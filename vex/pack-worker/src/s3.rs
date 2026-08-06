//! S3-compatible immutable pack transport with multipart uploads.

use std::path::Path;

use anyhow::{bail, Context, Result};
use aws_config::BehaviorVersion;
use aws_credential_types::Credentials;
use aws_sdk_s3::primitives::ByteStream;
use aws_sdk_s3::types::{CompletedMultipartUpload, CompletedPart};
use aws_sdk_s3::Client;
use aws_smithy_types::byte_stream::Length;
use aws_types::region::Region;

use crate::config::WorkerConfig;

const MEBIBYTE: u64 = 1024 * 1024;
const STREAM_BUFFER_BYTES: usize = (2 * MEBIBYTE) as usize;

/// Builds an authenticated S3 client from Vex's explicit service environment.
pub async fn client(config: &WorkerConfig) -> Client {
    let credentials = Credentials::new(
        config.s3_access_key.clone(),
        config.s3_secret_key.clone(),
        None,
        None,
        "vex-pack-worker",
    );
    let shared = aws_config::defaults(BehaviorVersion::latest())
        .region(Region::new(config.s3_region.clone()))
        .credentials_provider(credentials)
        .load()
        .await;
    let service = aws_sdk_s3::config::Builder::from(&shared)
        .endpoint_url(config.s3_endpoint.clone())
        .force_path_style(config.s3_path_style)
        .build();
    Client::from_conf(service)
}

/// Uploads one immutable artifact, using multipart S3 operations when required.
pub async fn upload_file(
    client: &Client,
    config: &WorkerConfig,
    path: &Path,
    key: &str,
) -> Result<()> {
    let bytes = std::fs::metadata(path)
        .with_context(|| format!("inspect S3 artifact {}", path.display()))?
        .len();
    if existing_object_matches(client, config, key, bytes).await? {
        return Ok(());
    }
    if bytes <= config.multipart_part_bytes {
        upload_single_part(client, config, path, key, bytes).await?;
    } else {
        upload_multipart(client, config, path, key, bytes).await?;
    }
    verify_object_size(client, config, key, bytes).await
}

async fn existing_object_matches(
    client: &Client,
    config: &WorkerConfig,
    key: &str,
    expected_bytes: u64,
) -> Result<bool> {
    match client
        .head_object()
        .bucket(&config.s3_bucket)
        .key(key)
        .send()
        .await
    {
        Ok(output) => {
            let actual = output
                .content_length()
                .context("S3 object response omitted content length")?;
            let actual =
                u64::try_from(actual).context("S3 object reported negative content length")?;
            if actual != expected_bytes {
                bail!(
					"immutable S3 key {key} already exists with {actual} bytes, expected {expected_bytes}"
				);
            }
            Ok(true)
        }
        Err(error)
            if error
                .as_service_error()
                .is_some_and(aws_sdk_s3::operation::head_object::HeadObjectError::is_not_found) =>
        {
            Ok(false)
        }
        Err(error) => Err(error).with_context(|| format!("inspect immutable S3 object {key}")),
    }
}

async fn upload_single_part(
    client: &Client,
    config: &WorkerConfig,
    path: &Path,
    key: &str,
    bytes: u64,
) -> Result<()> {
    let body = ByteStream::from_path(path)
        .await
        .with_context(|| format!("stream S3 artifact {}", path.display()))?;
    client
        .put_object()
        .bucket(&config.s3_bucket)
        .key(key)
        .content_length(i64::try_from(bytes).context("S3 artifact exceeds signed length range")?)
        .body(body)
        .send()
        .await
        .with_context(|| format!("upload immutable S3 object {key}"))?;
    Ok(())
}

async fn upload_multipart(
    client: &Client,
    config: &WorkerConfig,
    path: &Path,
    key: &str,
    bytes: u64,
) -> Result<()> {
    let part_count = bytes.div_ceil(config.multipart_part_bytes);
    if part_count > 10_000 {
        bail!("S3 multipart upload would exceed its 10,000-part limit");
    }
    let created = client
        .create_multipart_upload()
        .bucket(&config.s3_bucket)
        .key(key)
        .send()
        .await
        .with_context(|| format!("start multipart upload for {key}"))?;
    let upload_id = created
        .upload_id()
        .context("S3 multipart creation omitted upload ID")?
        .to_owned();
    let result = upload_multipart_parts(client, config, path, key, bytes, &upload_id).await;
    if let Err(error) = result {
        let _ = client
            .abort_multipart_upload()
            .bucket(&config.s3_bucket)
            .key(key)
            .upload_id(upload_id)
            .send()
            .await;
        return Err(error);
    }
    Ok(())
}

async fn upload_multipart_parts(
    client: &Client,
    config: &WorkerConfig,
    path: &Path,
    key: &str,
    bytes: u64,
    upload_id: &str,
) -> Result<()> {
    let mut parts = Vec::new();
    let mut offset = 0_u64;
    let mut part_number = 1_i32;
    while offset < bytes {
        let length = (bytes - offset).min(config.multipart_part_bytes);
        let body = ByteStream::read_from()
            .path(path)
            .offset(offset)
            .length(Length::Exact(length))
            .buffer_size(STREAM_BUFFER_BYTES)
            .build()
            .await
            .with_context(|| format!("stream multipart S3 range at byte {offset}"))?;
        let uploaded = client
            .upload_part()
            .bucket(&config.s3_bucket)
            .key(key)
            .upload_id(upload_id)
            .part_number(part_number)
            .content_length(
                i64::try_from(length).context("multipart part exceeds signed length range")?,
            )
            .body(body)
            .send()
            .await
            .with_context(|| format!("upload S3 multipart part {part_number} for {key}"))?;
        let e_tag = uploaded
            .e_tag()
            .context("S3 multipart part response omitted ETag")?;
        parts.push(
            CompletedPart::builder()
                .part_number(part_number)
                .e_tag(e_tag)
                .build(),
        );
        offset += length;
        part_number = part_number
            .checked_add(1)
            .context("S3 multipart part number overflow")?;
    }

    let completed = CompletedMultipartUpload::builder()
        .set_parts(Some(parts))
        .build();
    client
        .complete_multipart_upload()
        .bucket(&config.s3_bucket)
        .key(key)
        .upload_id(upload_id)
        .multipart_upload(completed)
        .send()
        .await
        .with_context(|| format!("complete multipart upload for {key}"))?;
    Ok(())
}

async fn verify_object_size(
    client: &Client,
    config: &WorkerConfig,
    key: &str,
    expected_bytes: u64,
) -> Result<()> {
    let output = client
        .head_object()
        .bucket(&config.s3_bucket)
        .key(key)
        .send()
        .await
        .with_context(|| format!("verify immutable S3 object {key}"))?;
    let actual = output
        .content_length()
        .context("S3 verification response omitted content length")?;
    let actual =
        u64::try_from(actual).context("S3 verification reported negative content length")?;
    if actual != expected_bytes {
        bail!("S3 object {key} has {actual} bytes, expected {expected_bytes}");
    }
    Ok(())
}
