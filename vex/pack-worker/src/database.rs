//! Durable MySQL claims and fenced derived-pack publication.

use std::fs::File;
use std::io;

use anyhow::{bail, Context, Result};
use mysql::prelude::Queryable;
use mysql::{params, LocalInfileHandler, Opts, Pool, TxOpts};
use uuid::Uuid;

use crate::config::WorkerConfig;
use crate::model::{ClaimedJob, HashAlgorithm, PackArtifact, PublishOutcome};

const LOCAL_MANIFEST_NAME: &str = "vex-pack-worker-input.tsv";

/// Owns short MySQL transactions; pack construction and S3 transfer stay outside them.
pub struct Database {
    pool: Pool,
    lease_seconds: u64,
    retry_seconds: u64,
}

impl Database {
    /// Opens the worker's pool from the explicit Vex MySQL DSN.
    pub fn new(config: &WorkerConfig) -> Result<Self> {
        let options = Opts::from_url(&config.mysql_dsn).context("parse Vex MySQL DSN")?;
        let pool = Pool::new(options).context("create Vex MySQL pool")?;
        Ok(Self {
            pool,
            lease_seconds: config.lease_seconds,
            retry_seconds: config.retry_seconds,
        })
    }

    /// Claims one due request using `SKIP LOCKED`, or returns `None` when idle.
    pub fn claim_due(&self) -> Result<Option<ClaimedJob>> {
        let mut connection = self.pool.get_conn().context("get Vex MySQL connection")?;
        let mut transaction = connection
            .start_transaction(TxOpts::default())
            .context("begin Vex pack-worker claim transaction")?;
        let candidate: Option<(Vec<u8>, Vec<u8>, String, u64)> = transaction
            .exec_first(
                "SELECT q.repo_id, r.name, r.hash_algo, q.target_generation \
				 FROM pack_rebuild_requests AS q \
				 JOIN repositories AS r ON r.repo_id = q.repo_id \
				 WHERE r.name IS NOT NULL \
				   AND q.not_before <= CURRENT_TIMESTAMP(6) \
				   AND (q.worker_lease_expires_at IS NULL \
				        OR q.worker_lease_expires_at < CURRENT_TIMESTAMP(6)) \
				 ORDER BY q.not_before, q.repo_id \
				 LIMIT 1 FOR UPDATE SKIP LOCKED",
                (),
            )
            .context("select due Vex pack rebuild request")?;
        let Some((repo_id, route, hash_algorithm, generation)) = candidate else {
            transaction
                .commit()
                .context("commit idle Vex worker claim")?;
            return Ok(None);
        };
        let repo_id: [u8; 16] = repo_id
            .try_into()
            .map_err(|_| anyhow::anyhow!("MySQL returned an invalid Vex repository UUID"))?;
        let repository_name =
            String::from_utf8(route).context("MySQL returned a non-UTF-8 Vex repository route")?;
        let hash_algorithm = HashAlgorithm::parse(&hash_algorithm)?;
        let lease_id = Uuid::new_v4();
        let claim_sql = format!(
            "UPDATE pack_rebuild_requests SET \
			 worker_lease_id = :lease_id, \
			 claimed_target_generation = target_generation, \
			 worker_lease_expires_at = DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL {} SECOND), \
			 worker_claimed_at = CURRENT_TIMESTAMP(6), \
			 attempts = attempts + 1, \
			 last_error = NULL \
			 WHERE repo_id = :repo_id",
            self.lease_seconds
        );
        transaction
            .exec_drop(
                claim_sql,
                params! {
                    "repo_id" => repo_id.to_vec(),
                    "lease_id" => lease_id.as_bytes().to_vec(),
                },
            )
            .context("claim Vex pack rebuild request")?;
        if transaction.affected_rows() != 1 {
            bail!("Vex pack rebuild request disappeared during its row lock");
        }
        transaction
            .commit()
            .context("commit Vex pack-worker claim")?;
        Ok(Some(ClaimedJob {
            repo_id,
            repository_name,
            hash_algorithm,
            generation,
            lease_id,
        }))
    }

    /// Detects the raw-WAL format that this first derived worker intentionally does not compact.
    pub fn has_raw_receive_pack(&self, job: &ClaimedJob) -> Result<bool> {
        let mut connection = self.pool.get_conn().context("get Vex MySQL connection")?;
        let count: Option<u64> = connection
            .exec_first(
                "SELECT COUNT(*) FROM pack_envelopes \
				 WHERE repo_id = :repo_id \
				   AND source_kind = 'receive' \
				   AND state = 'sealed'",
                params! { "repo_id" => job.repo_id.to_vec() },
            )
            .context("inspect raw Vex receive packs")?;
        Ok(count.context("raw Vex receive count was missing")? != 0)
    }

    /// Extends a held lease after a bounded build or upload phase.
    ///
    /// A `false` result means the queued target advanced or another worker took
    /// the request. Both cases are stale rather than retryable failures.
    pub fn renew_lease(&self, job: &ClaimedJob) -> Result<bool> {
        let mut connection = self.pool.get_conn().context("get Vex MySQL connection")?;
        let sql = format!(
            "UPDATE pack_rebuild_requests SET \
			 worker_lease_expires_at = DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL {} SECOND) \
				 WHERE repo_id = :repo_id \
				   AND worker_lease_id = :lease_id \
				   AND claimed_target_generation = :generation \
				   AND target_generation = :generation",
            self.lease_seconds
        );
        connection
            .exec_drop(
                sql,
                params! {
                    "repo_id" => job.repo_id.to_vec(),
                    "lease_id" => job.lease_id.as_bytes().to_vec(),
                    "generation" => job.generation,
                },
            )
            .context("renew Vex pack-worker lease")?;
        Ok(connection.affected_rows() == 1)
    }

    /// Releases a claim after a retryable failure without changing its target generation.
    pub fn release_for_retry(&self, job: &ClaimedJob, error: &str) -> Result<()> {
        let mut connection = self.pool.get_conn().context("get Vex MySQL connection")?;
        let sql = format!(
            "UPDATE pack_rebuild_requests SET \
			 worker_lease_id = NULL, \
			 claimed_target_generation = NULL, \
			 worker_lease_expires_at = NULL, \
			 worker_claimed_at = NULL, \
			 last_error = :last_error, \
			 not_before = DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL {} SECOND) \
			 WHERE repo_id = :repo_id AND worker_lease_id = :lease_id",
            self.retry_seconds
        );
        connection
            .exec_drop(
                sql,
                params! {
                    "repo_id" => job.repo_id.to_vec(),
                    "lease_id" => job.lease_id.as_bytes().to_vec(),
                    "last_error" => bounded_error(error),
                },
            )
            .context("release failed Vex pack-worker lease")?;
        Ok(())
    }

    /// Releases a stale claim immediately so the newer debounced target can run.
    pub fn release_stale(&self, job: &ClaimedJob) -> Result<()> {
        let mut connection = self.pool.get_conn().context("get Vex MySQL connection")?;
        connection
            .exec_drop(
                "UPDATE pack_rebuild_requests SET \
				 worker_lease_id = NULL, \
				 claimed_target_generation = NULL, \
				 worker_lease_expires_at = NULL, \
				 worker_claimed_at = NULL, \
				 last_error = :last_error \
				 WHERE repo_id = :repo_id AND worker_lease_id = :lease_id",
                params! {
                    "repo_id" => job.repo_id.to_vec(),
                    "lease_id" => job.lease_id.as_bytes().to_vec(),
                    "last_error" => format!("stale derived-pack target {}", job.generation),
                },
            )
            .context("release stale Vex pack-worker lease")?;
        Ok(())
    }

    /// Inserts an immutable derived pack only while the claimed sealed generation remains current.
    pub fn publish_artifact(
        &self,
        job: &ClaimedJob,
        artifact: &PackArtifact,
    ) -> Result<PublishOutcome> {
        let mut connection = self.pool.get_conn().context("get Vex MySQL connection")?;
        let mut transaction = connection
            .start_transaction(TxOpts::default())
            .context("begin Vex pack publication transaction")?;
        let repository: Option<Vec<u8>> = transaction
            .exec_first(
                "SELECT repo_id FROM repositories WHERE repo_id = :repo_id FOR UPDATE",
                params! { "repo_id" => job.repo_id.to_vec() },
            )
            .context("lock Vex repository for pack publication")?;
        if repository.is_none() {
            bail!("Vex repository vanished before pack publication");
        }
        let claim: Option<(u64, Option<u64>, Option<Vec<u8>>)> = transaction
            .exec_first(
                "SELECT target_generation, claimed_target_generation, worker_lease_id \
				 FROM pack_rebuild_requests WHERE repo_id = :repo_id FOR UPDATE",
                params! { "repo_id" => job.repo_id.to_vec() },
            )
            .context("lock Vex pack rebuild request for publication")?;
        let Some((target_generation, claimed_generation, lease_id)) = claim else {
            return Ok(PublishOutcome::Stale);
        };
        if target_generation != job.generation
            || claimed_generation != Some(job.generation)
            || lease_id.as_deref() != Some(job.lease_id.as_bytes())
        {
            return Ok(PublishOutcome::Stale);
        }
        let current_generation: Option<u64> = transaction
            .exec_first(
                "SELECT COALESCE(MAX(generation), 0) FROM generations \
				 WHERE repo_id = :repo_id AND state = 'sealed'",
                params! { "repo_id" => job.repo_id.to_vec() },
            )
            .context("read current Vex sealed generation")?;
        if current_generation.context("Vex sealed generation query returned no row")?
            != job.generation
        {
            return Ok(PublishOutcome::Stale);
        }
        let raw_receive_count: Option<u64> = transaction
            .exec_first(
                "SELECT COUNT(*) FROM pack_envelopes \
				 WHERE repo_id = :repo_id \
				   AND source_kind = 'receive' \
				   AND state = 'sealed'",
                params! { "repo_id" => job.repo_id.to_vec() },
            )
            .context("recheck raw Vex receive packs")?;
        if raw_receive_count.context("raw Vex receive count was missing")? != 0 {
            bail!("raw receive-pack compaction needs the later raw-member publication path");
        }

        let (pack_key, index_key) = artifact_keys(job, artifact);
        let existing: Option<(String, Vec<u8>, Option<Vec<u8>>)> = transaction
            .exec_first(
                "SELECT source_kind, pack_key, idx_key FROM pack_envelopes \
				 WHERE repo_id = :repo_id AND pack_id = :pack_id FOR UPDATE",
                params! {
                    "repo_id" => job.repo_id.to_vec(),
                    "pack_id" => artifact.pack_id_binary.clone(),
                },
            )
            .context("inspect existing Vex pack envelope")?;
        match existing {
            None => {
                transaction
                    .exec_drop(
                        "INSERT INTO pack_envelopes \
						 (repo_id, pack_id, pack_key, idx_key, bitmap_key, covering_generation, \
						  state, source_kind, receive_generation, is_full_snapshot, sealed_at) \
						 VALUES (:repo_id, :pack_id, :pack_key, :idx_key, NULL, :generation, \
						  'sealed', 'derived', NULL, TRUE, CURRENT_TIMESTAMP(6))",
                        params! {
                            "repo_id" => job.repo_id.to_vec(),
                            "pack_id" => artifact.pack_id_binary.clone(),
                            "pack_key" => pack_key.as_bytes().to_vec(),
                            "idx_key" => index_key.as_bytes().to_vec(),
                            "generation" => job.generation,
                        },
                    )
                    .context("insert Vex derived pack envelope")?;
            }
            Some((source_kind, existing_pack_key, Some(existing_index_key)))
                if source_kind == "derived"
                    && existing_pack_key == pack_key.as_bytes()
                    && existing_index_key == index_key.as_bytes() =>
            {
                transaction
                    .exec_drop(
                        "UPDATE pack_envelopes SET covering_generation = :generation, \
						 state = 'sealed', sealed_at = CURRENT_TIMESTAMP(6), obsolete_at = NULL \
						 WHERE repo_id = :repo_id AND pack_id = :pack_id",
                        params! {
                            "repo_id" => job.repo_id.to_vec(),
                            "pack_id" => artifact.pack_id_binary.clone(),
                            "generation" => job.generation,
                        },
                    )
                    .context("refresh Vex derived pack envelope")?;
            }
            Some(_) => bail!("immutable Vex pack identifier collides with a different envelope"),
        }

        transaction
            .exec_drop(
                "DELETE FROM pack_objects WHERE repo_id = :repo_id AND pack_id = :pack_id",
                params! {
                    "repo_id" => job.repo_id.to_vec(),
                    "pack_id" => artifact.pack_id_binary.clone(),
                },
            )
            .context("replace Vex derived pack membership")?;
        transaction
            .exec_drop(
                "DELETE FROM pack_object_offsets WHERE repo_id = :repo_id AND pack_id = :pack_id",
                params! {
                    "repo_id" => job.repo_id.to_vec(),
                    "pack_id" => artifact.pack_id_binary.clone(),
                },
            )
            .context("replace Vex derived pack offsets")?;
        load_pack_manifest(&mut transaction, artifact)?;
        let imported: Option<u64> = transaction
            .query_first("SELECT COUNT(*) FROM vex_pack_worker_input")
            .context("count Vex pack offset manifest rows")?;
        if imported.context("Vex pack offset manifest count was missing")? != artifact.object_count
        {
            bail!("Vex pack offset manifest count differs from the Git pack index");
        }
        transaction
            .exec_drop(
                "INSERT INTO pack_objects (repo_id, pack_id, oid) \
				 SELECT :repo_id, :pack_id, oid FROM vex_pack_worker_input",
                params! {
                    "repo_id" => job.repo_id.to_vec(),
                    "pack_id" => artifact.pack_id_binary.clone(),
                },
            )
            .context("bulk insert Vex pack membership")?;
        transaction
            .exec_drop(
                "INSERT INTO pack_object_offsets (repo_id, pack_id, oid, pack_offset) \
				 SELECT :repo_id, :pack_id, oid, pack_offset FROM vex_pack_worker_input",
                params! {
                    "repo_id" => job.repo_id.to_vec(),
                    "pack_id" => artifact.pack_id_binary.clone(),
                },
            )
            .context("bulk insert Vex pack offsets")?;
        transaction
            .query_drop("DROP TEMPORARY TABLE vex_pack_worker_input")
            .context("drop Vex pack offset import table")?;
        transaction
            .exec_drop(
                "DELETE FROM pack_rebuild_requests \
				 WHERE repo_id = :repo_id \
				   AND worker_lease_id = :lease_id \
				   AND target_generation = :generation \
				   AND claimed_target_generation = :generation",
                params! {
                    "repo_id" => job.repo_id.to_vec(),
                    "lease_id" => job.lease_id.as_bytes().to_vec(),
                    "generation" => job.generation,
                },
            )
            .context("clear published Vex pack rebuild request")?;
        if transaction.affected_rows() != 1 {
            return Ok(PublishOutcome::Stale);
        }
        transaction
            .commit()
            .context("commit Vex derived pack publication")?;
        Ok(PublishOutcome::Published)
    }
}

fn load_pack_manifest(
    transaction: &mut mysql::Transaction<'_>,
    artifact: &PackArtifact,
) -> Result<()> {
    transaction
        .query_drop("DROP TEMPORARY TABLE IF EXISTS vex_pack_worker_input")
        .context("clear stale Vex pack offset import table")?;
    transaction
        .query_drop(
            "CREATE TEMPORARY TABLE vex_pack_worker_input (\
			 oid BINARY(32) NOT NULL, \
			 pack_offset BIGINT UNSIGNED NOT NULL, \
			 PRIMARY KEY (oid)\
			) ENGINE=InnoDB",
        )
        .context("create Vex pack offset import table")?;
    let manifest = artifact.offsets_path.clone();
    transaction.set_local_infile_handler(Some(LocalInfileHandler::new(
        move |file_name, writer| {
            if file_name != LOCAL_MANIFEST_NAME.as_bytes() {
                return Err(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    "unexpected Vex local-infile request",
                ));
            }
            let mut file = File::open(&manifest)?;
            io::copy(&mut file, writer)?;
            Ok(())
        },
    )));
    let loaded = transaction
        .query_drop(
            "LOAD DATA LOCAL INFILE 'vex-pack-worker-input.tsv' \
			 INTO TABLE vex_pack_worker_input \
			 FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' \
			 (@oid_hex, pack_offset) SET oid = UNHEX(@oid_hex)",
        )
        .context("bulk load Vex pack offsets");
    transaction.set_local_infile_handler(None);
    loaded
}

/// Returns the immutable S3 key pair which publication will bind to the pack.
pub fn artifact_keys(job: &ClaimedJob, artifact: &PackArtifact) -> (String, String) {
    (
        format!(
            "git/v1/{}/packs/{}.pack",
            job.repo_id_hex(),
            artifact.pack_id_hex
        ),
        format!(
            "git/v1/{}/packs/{}.idx",
            job.repo_id_hex(),
            artifact.pack_id_hex
        ),
    )
}

fn bounded_error(error: &str) -> String {
    error.chars().take(1024).collect()
}

#[cfg(test)]
mod tests {
    use super::bounded_error;

    #[test]
    fn bounds_retry_errors_to_the_schema_limit() {
        assert_eq!(bounded_error(&"x".repeat(1025)).chars().count(), 1024);
    }
}
