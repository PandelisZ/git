//! Pack-granularity orchestration with a fenced metadata publication boundary.

use anyhow::{Context, Result};

use crate::config::WorkerConfig;
use crate::database::{artifact_keys, Database};
use crate::git::{build_pack, resolve_repository};
use crate::model::{ClaimedJob, PublishOutcome};
use crate::s3;

/// Executes one durable rebuild request at a time.
///
/// Git's C receive path remains the sole owner of refs, ref audit records,
/// generations, and canonical object bytes. This worker can only add a
/// derived immutable pack after it proves the claimed sealed generation is
/// still current.
pub struct Worker {
    config: WorkerConfig,
    database: Database,
    s3_client: aws_sdk_s3::Client,
}

impl Worker {
    /// Connects the two Vex storage transports before accepting work.
    pub async fn new(config: WorkerConfig) -> Result<Self> {
        let database = Database::new(&config)?;
        let s3_client = s3::client(&config).await;
        Ok(Self {
            config,
            database,
            s3_client,
        })
    }

    /// Returns the continuous-mode idle poll interval.
    pub const fn poll_seconds(&self) -> u64 {
        self.config.poll_seconds
    }

    /// Claims and handles at most one rebuild. `false` means no job was ready.
    pub async fn run_once(&self) -> Result<bool> {
        let Some(job) = self.database.claim_due()? else {
            return Ok(false);
        };

        match self.process_claim(&job).await {
            Ok(PublishOutcome::Published) => {}
            Ok(PublishOutcome::Stale) => {
                self.database.release_stale(&job)?;
                println!(
                    "vex-pack-worker: stale repo={} generation={}",
                    job.repository_name, job.generation
                );
            }
            Err(error) => {
                let message = format!("{error:#}");
                self.database.release_for_retry(&job, &message)?;
                return Err(error);
            }
        }
        Ok(true)
    }

    async fn process_claim(&self, job: &ClaimedJob) -> Result<PublishOutcome> {
        // The received-pack WAL has its own raw member map. The first worker is
        // deliberately constrained to the mature per-object canonical path,
        // where derived `pack_objects` membership can preserve its object FK.
        if self.database.has_raw_receive_pack(job)? {
            anyhow::bail!(
                "raw receive-pack compaction is not enabled in this worker phase; \
				 use the raw pack publication path"
            );
        }
        if !self.database.renew_lease(job)? {
            return Ok(PublishOutcome::Stale);
        }

        let repository = resolve_repository(&self.config, job)?;
        let built = build_pack(&self.config, job, &repository)?;
        if !self.database.renew_lease(job)? {
            return Ok(PublishOutcome::Stale);
        }

        let (pack_key, index_key) = artifact_keys(job, &built.artifact);
        s3::upload_file(
            &self.s3_client,
            &self.config,
            &built.artifact.pack_path,
            &pack_key,
        )
        .await
        .with_context(|| format!("upload derived Vex pack {pack_key}"))?;
        s3::upload_file(
            &self.s3_client,
            &self.config,
            &built.artifact.index_path,
            &index_key,
        )
        .await
        .with_context(|| format!("upload derived Vex index {index_key}"))?;
        if !self.database.renew_lease(job)? {
            return Ok(PublishOutcome::Stale);
        }

        let outcome = self.database.publish_artifact(job, &built.artifact)?;
        if outcome == PublishOutcome::Published {
            println!(
                "vex-pack-worker: published repository={} generation={} pack={} objects={}",
                job.repository_name,
                job.generation,
                built.artifact.pack_id_hex,
                built.artifact.object_count
            );
        }
        Ok(outcome)
    }
}
