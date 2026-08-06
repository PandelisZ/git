//! Domain types that keep the worker's Git and metadata boundaries explicit.

use std::path::PathBuf;

use anyhow::{bail, Result};
use uuid::Uuid;

/// The object hash family configured by the owning Vex repository.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HashAlgorithm {
    /// SHA-1 Git object identifiers are stored zero-padded to 32 bytes in MySQL.
    Sha1,
    /// SHA-256 Git object identifiers already fill MySQL's 32-byte binary columns.
    Sha256,
}

impl HashAlgorithm {
    /// Parses the exact repository value stored by the Vex schema.
    pub fn parse(value: &str) -> Result<Self> {
        match value {
            "sha1" => Ok(Self::Sha1),
            "sha256" => Ok(Self::Sha256),
            _ => bail!("unsupported Vex repository hash algorithm {value:?}"),
        }
    }

    /// Returns the textual Git object ID width for this repository.
    pub const fn hex_length(self) -> usize {
        match self {
            Self::Sha1 => 40,
            Self::Sha256 => 64,
        }
    }
}

/// A rebuild request claimed from MySQL by one worker lease.
#[derive(Clone, Debug)]
pub struct ClaimedJob {
    pub repo_id: [u8; 16],
    pub repository_name: String,
    pub hash_algorithm: HashAlgorithm,
    pub generation: u64,
    pub lease_id: Uuid,
}

impl ClaimedJob {
    /// Returns the canonical lowercase metadata namespace for the repository.
    pub fn repo_id_hex(&self) -> String {
        hex::encode(self.repo_id)
    }
}

/// A verified immutable pack/index pair plus its streamed offset manifest.
#[derive(Clone, Debug)]
pub struct PackArtifact {
    pub pack_path: PathBuf,
    pub index_path: PathBuf,
    pub offsets_path: PathBuf,
    pub pack_id_hex: String,
    pub pack_id_binary: Vec<u8>,
    pub object_count: u64,
    pub pack_bytes: u64,
    pub index_bytes: u64,
}

/// The final publication result after the repository row is fenced.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PublishOutcome {
    /// The derived envelope and offset metadata became visible atomically.
    Published,
    /// A later receive moved the target before this worker could publish.
    Stale,
}

/// Pads a repository-format Git object ID into Vex's BINARY(32) storage shape.
pub fn padded_oid(hex_oid: &str, algorithm: HashAlgorithm) -> Result<Vec<u8>> {
    if hex_oid.len() != algorithm.hex_length()
        || !hex_oid.bytes().all(|byte| byte.is_ascii_hexdigit())
    {
        bail!("invalid {:?} object identifier", algorithm);
    }
    let mut value = hex::decode(hex_oid)?;
    value.resize(32, 0);
    Ok(value)
}

#[cfg(test)]
mod tests {
    use super::{padded_oid, HashAlgorithm};

    #[test]
    fn pads_sha1_object_ids_for_mysql() {
        let oid = "a".repeat(40);
        let padded = padded_oid(&oid, HashAlgorithm::Sha1).expect("valid SHA-1 OID");
        assert_eq!(padded.len(), 32);
        assert!(padded[20..].iter().all(|byte| *byte == 0));
    }

    #[test]
    fn preserves_sha256_object_ids_for_mysql() {
        let oid = "b".repeat(64);
        let padded = padded_oid(&oid, HashAlgorithm::Sha256).expect("valid SHA-256 OID");
        assert_eq!(padded.len(), 32);
    }
}
