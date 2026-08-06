//! Environment and command-line configuration for the pack worker.

use std::env;
use std::path::PathBuf;

use anyhow::{bail, Context, Result};
use clap::Parser;

const MEBIBYTE: u64 = 1024 * 1024;
const MIN_MULTIPART_PART_BYTES: u64 = 5 * MEBIBYTE;
const DEFAULT_MULTIPART_PART_BYTES: u64 = 64 * MEBIBYTE;
const DEFAULT_MULTIPART_CONCURRENCY: u64 = 8;
const MAX_MULTIPART_CONCURRENCY: u64 = 33;
const DEFAULT_LEASE_SECONDS: u64 = 15 * 60;
const DEFAULT_POLL_SECONDS: u64 = 5;
const DEFAULT_RETRY_SECONDS: u64 = 30;

/// Runs one durable pack rebuild or continuously polls for rebuilds.
#[derive(Debug, Parser)]
#[command(name = "vex-pack-worker", version, about)]
pub struct Cli {
    /// Claim and process at most one ready rebuild request, then exit.
    #[arg(long)]
    pub once: bool,

    /// Seconds between no-work polls in continuous mode.
    #[arg(long, value_name = "SECONDS")]
    pub poll_seconds: Option<u64>,
}

/// Immutable process configuration. Secrets are intentionally not `Debug`.
#[derive(Clone)]
pub struct WorkerConfig {
    pub mysql_dsn: String,
    pub s3_endpoint: String,
    pub s3_bucket: String,
    pub s3_region: String,
    pub s3_path_style: bool,
    pub s3_access_key: String,
    pub s3_secret_key: String,
    pub git_root: PathBuf,
    pub scratch_root: PathBuf,
    pub git_binary: PathBuf,
    pub lease_seconds: u64,
    pub retry_seconds: u64,
    pub poll_seconds: u64,
    pub multipart_part_bytes: u64,
    /// Maximum number of streaming S3 multipart part uploads per artifact.
    pub multipart_concurrency: usize,
}

impl WorkerConfig {
    /// Reads the Vex Compose contract without falling back to ambient AWS state.
    pub fn from_env(cli: &Cli) -> Result<Self> {
        let multipart_part_bytes = read_u64(
            "VEX_PACK_WORKER_MULTIPART_PART_BYTES",
            DEFAULT_MULTIPART_PART_BYTES,
            MIN_MULTIPART_PART_BYTES,
            5 * 1024 * 1024 * 1024,
        )?;
        let multipart_concurrency = usize::try_from(read_u64(
            "VEX_PACK_WORKER_MULTIPART_CONCURRENCY",
            DEFAULT_MULTIPART_CONCURRENCY,
            1,
            MAX_MULTIPART_CONCURRENCY,
        )?)
        .context("VEX_PACK_WORKER_MULTIPART_CONCURRENCY does not fit this platform")?;
        let s3_endpoint = required_env("GIT_VEX_S3_ENDPOINT")?;
        if !(s3_endpoint.starts_with("http://") || s3_endpoint.starts_with("https://")) {
            bail!("GIT_VEX_S3_ENDPOINT must be an http(s) endpoint");
        }

        let poll_seconds = match cli.poll_seconds {
            Some(value) if (1..=60 * 60).contains(&value) => value,
            Some(_) => bail!("--poll-seconds must be between 1 and 3600"),
            None => read_u64(
                "VEX_PACK_WORKER_POLL_SECONDS",
                DEFAULT_POLL_SECONDS,
                1,
                60 * 60,
            )?,
        };

        Ok(Self {
            mysql_dsn: required_env("GIT_VEX_MYSQL_DSN")?,
            s3_endpoint,
            s3_bucket: required_env("GIT_VEX_S3_BUCKET")?,
            s3_region: required_env("GIT_VEX_S3_REGION")?,
            s3_path_style: read_bool("GIT_VEX_S3_PATH_STYLE", true)?,
            s3_access_key: required_env("GIT_VEX_S3_ACCESS_KEY")?,
            s3_secret_key: required_env("GIT_VEX_S3_SECRET_KEY")?,
            git_root: PathBuf::from(optional_env("GIT_HTTP_ROOT", "/srv/git")),
            scratch_root: PathBuf::from(optional_env("GIT_VEX_SCRATCH", "/var/git-scratch")),
            git_binary: PathBuf::from(optional_env("VEX_PACK_WORKER_GIT", "/opt/vex-git/git")),
            lease_seconds: read_u64(
                "VEX_PACK_WORKER_LEASE_SECONDS",
                DEFAULT_LEASE_SECONDS,
                30,
                24 * 60 * 60,
            )?,
            retry_seconds: read_u64(
                "VEX_PACK_WORKER_RETRY_SECONDS",
                DEFAULT_RETRY_SECONDS,
                1,
                24 * 60 * 60,
            )?,
            poll_seconds,
            multipart_part_bytes,
            multipart_concurrency,
        })
    }
}

fn required_env(name: &str) -> Result<String> {
    let value = env::var(name).with_context(|| format!("{name} is required"))?;
    if value.is_empty() || value.contains(['\r', '\n']) {
        bail!("{name} must be nonempty and single-line");
    }
    Ok(value)
}

fn optional_env(name: &str, default: &str) -> String {
    env::var(name).unwrap_or_else(|_| default.to_owned())
}

fn read_bool(name: &str, default: bool) -> Result<bool> {
    let value = match env::var(name) {
        Ok(value) => value,
        Err(_) => return Ok(default),
    };
    match value.as_str() {
        "1" | "true" | "yes" => Ok(true),
        "0" | "false" | "no" => Ok(false),
        _ => bail!("{name} must be one of 1, 0, true, false, yes, or no"),
    }
}

fn read_u64(name: &str, default: u64, minimum: u64, maximum: u64) -> Result<u64> {
    let value = match env::var(name) {
        Ok(value) => value,
        Err(_) => return Ok(default),
    };
    let parsed = value
        .parse::<u64>()
        .with_context(|| format!("{name} must be an unsigned integer"))?;
    if parsed < minimum || parsed > maximum {
        bail!("{name} must be between {minimum} and {maximum}");
    }
    Ok(parsed)
}

#[cfg(test)]
mod tests {
    use super::read_bool;

    #[test]
    fn accepts_compose_boolean_spellings() {
        std::env::set_var("VEX_PACK_WORKER_TEST_BOOL", "yes");
        assert!(read_bool("VEX_PACK_WORKER_TEST_BOOL", false).is_ok_and(|value| value));
        std::env::remove_var("VEX_PACK_WORKER_TEST_BOOL");
    }
}
