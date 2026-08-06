//! Safe invocation of the source-built Vex Git binary at pack granularity.

use std::fs::{self, File};
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use anyhow::{bail, Context, Result};
use uuid::Uuid;

use crate::config::WorkerConfig;
use crate::model::{padded_oid, ClaimedJob, PackArtifact};

/// Keeps a scratch directory alive until all pack bytes and manifests are published.
#[derive(Debug)]
pub struct BuiltPack {
    pub artifact: PackArtifact,
    _scratch: ScratchDirectory,
}

#[derive(Debug)]
struct ScratchDirectory {
    path: PathBuf,
}

impl ScratchDirectory {
    fn create(root: &Path, job: &ClaimedJob) -> Result<Self> {
        fs::create_dir_all(root)
            .with_context(|| format!("create Vex scratch root {}", root.display()))?;
        let path = root.join(format!(
            "vex-pack-worker-{}-{}",
            job.repo_id_hex(),
            Uuid::new_v4()
        ));
        fs::create_dir(&path)
            .with_context(|| format!("create pack worker scratch directory {}", path.display()))?;
        Ok(Self { path })
    }
}

impl Drop for ScratchDirectory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

/// Resolves a database route below the Git root and verifies its Vex identity.
pub fn resolve_repository(config: &WorkerConfig, job: &ClaimedJob) -> Result<PathBuf> {
    validate_repository_name(&job.repository_name)?;
    let git_root = fs::canonicalize(&config.git_root)
        .with_context(|| format!("resolve Git root {}", config.git_root.display()))?;
    let repository = fs::canonicalize(git_root.join(&job.repository_name)).with_context(|| {
        format!(
            "resolve Vex repository route {} below {}",
            job.repository_name,
            git_root.display()
        )
    })?;
    if !repository.starts_with(&git_root) {
        bail!("repository route resolves outside the configured Git root");
    }
    if !repository.is_dir() {
        bail!("Vex repository route is not a bare Git directory");
    }

    let output = git_command(config, job, &repository)
        .args(["config", "--get", "extensions.refstorage"])
        .output()
        .context("read Vex repository ref-storage configuration")?;
    if !output.status.success() {
        bail!(
            "Vex repository identity command exited with {}",
            output.status
        );
    }
    let configured = String::from_utf8(output.stdout)
        .context("Vex repository ref-storage configuration was not UTF-8")?;
    let expected = format!("vex://{}", job.repo_id_hex());
    if configured.trim_end() != expected {
        bail!("Vex repository ref-storage UUID does not match the claimed job");
    }
    Ok(repository)
}

/// Builds and locally verifies a complete derived pack once for a claimed job.
pub fn build_pack(config: &WorkerConfig, job: &ClaimedJob, repository: &Path) -> Result<BuiltPack> {
    let scratch = ScratchDirectory::create(&config.scratch_root, job)?;
    let oids_path = scratch.path.join("oids");
    let pack_path = scratch.path.join("pack");
    let index_path = scratch.path.join("pack.idx");
    let offsets_path = scratch.path.join("pack-offsets.tsv");

    let mut rev_list = git_command(config, job, repository);
    rev_list.args(["rev-list", "--objects", "--no-object-names", "--all"]);
    run_to_file(
        &mut rev_list,
        &oids_path,
        "enumerate Vex pack object closure",
    )?;
    if fs::metadata(&oids_path)?.len() == 0 {
        bail!("ref closure is empty; refusing to publish an empty derived pack");
    }

    let oids = File::open(&oids_path).context("open Vex pack object closure")?;
    let mut pack_objects = git_command(config, job, repository);
    pack_objects
        .args([
            "pack-objects",
            "--stdout",
            "--revs",
            "--no-reuse-object",
            "--no-reuse-delta",
            "--threads=4",
            "--window-memory=256m",
        ])
        .stdin(Stdio::from(oids));
    run_to_file(&mut pack_objects, &pack_path, "build Vex derived pack")?;

    let output = git_command(config, job, repository)
        .args(["index-pack", "-o"])
        .arg(&index_path)
        .arg(&pack_path)
        .output()
        .context("index Vex derived pack")?;
    if !output.status.success() {
        bail!("index Vex derived pack exited with {}", output.status);
    }
    let pack_id_hex = parse_pack_id(&output.stdout, job.hash_algorithm.hex_length())?;
    let pack_id_binary = padded_oid(&pack_id_hex, job.hash_algorithm)?;
    let object_count = write_pack_offsets(config, job, repository, &index_path, &offsets_path)?;
    if object_count == 0 {
        bail!("Vex pack index has no object offsets");
    }

    let artifact = PackArtifact {
        pack_path: pack_path.clone(),
        index_path: index_path.clone(),
        offsets_path,
        pack_id_hex,
        pack_id_binary,
        object_count,
        pack_bytes: fs::metadata(pack_path)?.len(),
        index_bytes: fs::metadata(index_path)?.len(),
    };
    if artifact.pack_bytes == 0 || artifact.index_bytes == 0 {
        bail!("Vex derived pack or its index is empty");
    }
    Ok(BuiltPack {
        artifact,
        _scratch: scratch,
    })
}

fn git_command(config: &WorkerConfig, job: &ClaimedJob, repository: &Path) -> Command {
    let mut command = Command::new(&config.git_binary);
    command
        .env("GIT_VEX_REPOSITORY_NAME", &job.repository_name)
        .arg(format!("--git-dir={}", repository.display()));
    command
}

fn run_to_file(command: &mut Command, output_path: &Path, description: &str) -> Result<()> {
    let output = File::create(output_path)
        .with_context(|| format!("create {} output {}", description, output_path.display()))?;
    let status = command
        .stdout(Stdio::from(output))
        .status()
        .with_context(|| description.to_owned())?;
    if !status.success() {
        bail!("{description} exited with {status}");
    }
    Ok(())
}

fn parse_pack_id(output: &[u8], expected_length: usize) -> Result<String> {
    let output = std::str::from_utf8(output).context("index-pack output was not UTF-8")?;
    let pack_id = output
        .split_whitespace()
        .rev()
        .find(|candidate| is_hex_oid(candidate, expected_length))
        .context("index-pack did not report a valid pack identifier")?;
    Ok(pack_id.to_ascii_lowercase())
}

fn write_pack_offsets(
    config: &WorkerConfig,
    job: &ClaimedJob,
    repository: &Path,
    index_path: &Path,
    offsets_path: &Path,
) -> Result<u64> {
    let index = File::open(index_path)
        .with_context(|| format!("open pack index {}", index_path.display()))?;
    let mut child = git_command(config, job, repository)
        .arg("show-index")
        .stdin(Stdio::from(index))
        .stdout(Stdio::piped())
        .spawn()
        .context("read Vex pack index offsets")?;
    let stdout = child.stdout.take().context("capture show-index output")?;
    let mut offsets = BufWriter::new(
        File::create(offsets_path)
            .with_context(|| format!("create pack offset manifest {}", offsets_path.display()))?,
    );
    let mut count = 0_u64;
    for line in BufReader::new(stdout).lines() {
        let line = line.context("read show-index output")?;
        let mut fields = line.split_ascii_whitespace();
        let offset = fields
            .next()
            .context("pack index row omitted its offset")?
            .parse::<u64>()
            .context("pack index reported an invalid object offset")?;
        let oid = fields
            .next()
            .context("pack index row omitted its object identifier")?;
        if !is_hex_oid(oid, job.hash_algorithm.hex_length()) {
            bail!("pack index reported an invalid object row");
        }
        writeln!(offsets, "{}\t{offset}", oid.to_ascii_lowercase())
            .context("write pack offset manifest")?;
        count = count.checked_add(1).context("pack object count overflow")?;
    }
    offsets.flush().context("flush pack offset manifest")?;
    let status = child.wait().context("wait for show-index")?;
    if !status.success() {
        bail!("read Vex pack index offsets exited with {status}");
    }
    Ok(count)
}

fn validate_repository_name(name: &str) -> Result<()> {
    if name.is_empty() || name.len() > 255 || !name.ends_with(".git") {
        bail!("invalid Vex repository route");
    }
    for component in name.split('/') {
        let mut characters = component.bytes();
        let Some(first) = characters.next() else {
            bail!("invalid empty Vex repository route component");
        };
        if !first.is_ascii_alphanumeric()
            || !characters
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
        {
            bail!("invalid Vex repository route component");
        }
    }
    Ok(())
}

fn is_hex_oid(value: &str, expected_length: usize) -> bool {
    value.len() == expected_length && value.bytes().all(|byte| byte.is_ascii_hexdigit())
}

#[cfg(test)]
mod tests {
    use super::{parse_pack_id, validate_repository_name};

    #[test]
    fn accepts_the_gateway_route_grammar() {
        assert!(validate_repository_name("team_1/repo-name.git").is_ok());
    }

    #[test]
    fn rejects_route_traversal() {
        assert!(validate_repository_name("team/../repo.git").is_err());
    }

    #[test]
    fn accepts_index_pack_output() {
        let output = b"pack\t0123456789abcdef0123456789abcdef01234567\n";
        assert_eq!(
            parse_pack_id(output, 40).expect("valid index-pack output"),
            "0123456789abcdef0123456789abcdef01234567"
        );
    }
}
