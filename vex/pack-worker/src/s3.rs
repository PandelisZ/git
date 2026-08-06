//! S3-compatible immutable pack transport with multipart uploads.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{bail, Context, Result};
use aws_config::BehaviorVersion;
use aws_credential_types::Credentials;
use aws_sdk_s3::primitives::ByteStream;
use aws_sdk_s3::types::{CompletedMultipartUpload, CompletedPart};
use aws_sdk_s3::Client;
use aws_smithy_types::byte_stream::Length;
use aws_types::region::Region;
use tokio::task::JoinSet;

use crate::config::WorkerConfig;

const MEBIBYTE: u64 = 1024 * 1024;
const STREAM_BUFFER_BYTES: usize = (2 * MEBIBYTE) as usize;
const MAX_MULTIPART_PARTS: u64 = 10_000;

#[derive(Clone)]
struct MultipartUploadContext {
    client: Client,
    bucket: String,
    path: PathBuf,
    key: String,
    upload_id: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct MultipartPart {
    part_number: i32,
    offset: u64,
    length: u64,
}

struct UploadedPart {
    part_number: i32,
    e_tag: String,
}

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
    let parts = multipart_parts(bytes, config.multipart_part_bytes)?;
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
    let result = upload_multipart_parts(
        client,
        &config.s3_bucket,
        path,
        key,
        &upload_id,
        parts,
        config.multipart_concurrency,
    )
    .await;
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
    bucket: &str,
    path: &Path,
    key: &str,
    upload_id: &str,
    parts: Vec<MultipartPart>,
    concurrency: usize,
) -> Result<()> {
    let context = Arc::new(MultipartUploadContext {
        client: client.clone(),
        bucket: bucket.to_owned(),
        path: path.to_path_buf(),
        key: key.to_owned(),
        upload_id: upload_id.to_owned(),
    });
    let mut remaining = parts.into_iter();
    let mut uploads = JoinSet::new();
    while uploads.len() < concurrency && spawn_next_part(&mut uploads, &context, &mut remaining) {}

    let mut uploaded = Vec::with_capacity(uploads.len());
    let result = loop {
        match uploads.join_next().await {
            Some(Ok(Ok(part))) => {
                uploaded.push(part);
                spawn_next_part(&mut uploads, &context, &mut remaining);
            }
            Some(Ok(Err(error))) => break Err(error),
            Some(Err(error)) => {
                break Err(anyhow::Error::new(error).context("multipart upload task failed"));
            }
            None => break Ok(uploaded),
        }
    };
    if result.is_err() {
        // Wait for cancellation before the outer layer issues AbortMultipartUpload.
        // This keeps a failed upload from racing a still-active UploadPart request.
        uploads.abort_all();
        while uploads.join_next().await.is_some() {}
    }
    let parts = complete_parts(result?);

    let completed = CompletedMultipartUpload::builder()
        .set_parts(Some(parts))
        .build();
    client
        .complete_multipart_upload()
        .bucket(bucket)
        .key(key)
        .upload_id(upload_id)
        .multipart_upload(completed)
        .send()
        .await
        .with_context(|| format!("complete multipart upload for {key}"))?;
    Ok(())
}

fn spawn_next_part(
    uploads: &mut JoinSet<Result<UploadedPart>>,
    context: &Arc<MultipartUploadContext>,
    parts: &mut impl Iterator<Item = MultipartPart>,
) -> bool {
    let Some(part) = parts.next() else {
        return false;
    };
    let context = Arc::clone(context);
    uploads.spawn(async move { upload_multipart_part(context, part).await });
    true
}

async fn upload_multipart_part(
    context: Arc<MultipartUploadContext>,
    part: MultipartPart,
) -> Result<UploadedPart> {
    let body = ByteStream::read_from()
        .path(&context.path)
        .offset(part.offset)
        .length(Length::Exact(part.length))
        .buffer_size(STREAM_BUFFER_BYTES)
        .build()
        .await
        .with_context(|| format!("stream multipart S3 range at byte {}", part.offset))?;
    let uploaded = context
        .client
        .upload_part()
        .bucket(&context.bucket)
        .key(&context.key)
        .upload_id(&context.upload_id)
        .part_number(part.part_number)
        .content_length(
            i64::try_from(part.length).context("multipart part exceeds signed length range")?,
        )
        .body(body)
        .send()
        .await
        .with_context(|| {
            format!(
                "upload S3 multipart part {} for {}",
                part.part_number, context.key
            )
        })?;
    let e_tag = uploaded
        .e_tag()
        .context("S3 multipart part response omitted ETag")?
        .to_owned();
    Ok(UploadedPart {
        part_number: part.part_number,
        e_tag,
    })
}

fn multipart_parts(bytes: u64, part_bytes: u64) -> Result<Vec<MultipartPart>> {
    let part_count = bytes.div_ceil(part_bytes);
    if part_count > MAX_MULTIPART_PARTS {
        bail!("S3 multipart upload would exceed its 10,000-part limit");
    }
    let capacity = usize::try_from(part_count).context("multipart part count exceeds platform")?;
    let mut parts = Vec::with_capacity(capacity);
    for index in 0..part_count {
        let offset = index
            .checked_mul(part_bytes)
            .context("multipart part offset overflow")?;
        let length = (bytes - offset).min(part_bytes);
        let part_number = i32::try_from(index + 1).context("multipart part number overflow")?;
        parts.push(MultipartPart {
            part_number,
            offset,
            length,
        });
    }
    Ok(parts)
}

fn complete_parts(mut uploaded: Vec<UploadedPart>) -> Vec<CompletedPart> {
    uploaded.sort_unstable_by_key(|part| part.part_number);
    uploaded
        .into_iter()
        .map(|part| {
            CompletedPart::builder()
                .part_number(part.part_number)
                .e_tag(part.e_tag)
                .build()
        })
        .collect()
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

#[cfg(test)]
mod tests {
    use super::{complete_parts, multipart_parts, UploadedPart};

    #[test]
    fn multipart_parts_cover_a_file_with_exact_offsets() {
        let parts = multipart_parts(13, 5).expect("part plan should be valid");
        let actual: Vec<_> = parts
            .iter()
            .map(|part| (part.part_number, part.offset, part.length))
            .collect();
        assert_eq!(actual, vec![(1, 0, 5), (2, 5, 5), (3, 10, 3)]);
    }

    #[test]
    fn multipart_parts_reject_more_than_s3_limit() {
        let error = multipart_parts(10_001, 1).expect_err("S3 has a 10,000-part limit");
        assert!(error.to_string().contains("10,000-part limit"));
    }

    #[test]
    fn complete_parts_orders_arrival_independent_of_upload_completion() {
        let parts = complete_parts(vec![
            UploadedPart {
                part_number: 3,
                e_tag: "third".to_owned(),
            },
            UploadedPart {
                part_number: 1,
                e_tag: "first".to_owned(),
            },
            UploadedPart {
                part_number: 2,
                e_tag: "second".to_owned(),
            },
        ]);
        let numbers: Vec<_> = parts.iter().map(|part| part.part_number()).collect();
        assert_eq!(numbers, vec![Some(1), Some(2), Some(3)]);
    }
}
