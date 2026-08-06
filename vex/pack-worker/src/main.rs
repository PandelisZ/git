//! Vex's pack-level derived-artifact worker.
//!
//! The worker claims durable rebuild requests, creates immutable Git pack/index
//! pairs, uploads their bytes to S3-compatible storage, and publishes only
//! derived pack metadata when its claimed generation is still current. It never
//! writes Git refs, ref audit rows, object bytes, or generations.

mod config;
mod database;
mod git;
mod model;
mod s3;
mod worker;

use std::time::Duration;

use anyhow::Result;
use clap::Parser;

use crate::config::{Cli, WorkerConfig};
use crate::worker::Worker;

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    let config = WorkerConfig::from_env(&cli)?;
    let worker = Worker::new(config).await?;

    if cli.once {
        if !worker.run_once().await? {
            println!("vex-pack-worker: idle");
        }
        return Ok(());
    }

    loop {
        if let Err(error) = worker.run_once().await {
            eprintln!("vex-pack-worker: job failed: {error:#}");
        }
        tokio::time::sleep(Duration::from_secs(worker.poll_seconds())).await;
    }
}
