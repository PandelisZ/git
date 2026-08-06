# Vex local storage topology

`compose.yaml` starts MySQL 8.4 and MinIO as the intended Vex metadata and
blob-store dependencies.  All host ports explicitly bind to `127.0.0.1`.

Run:

```sh
scripts/vex/up.sh
scripts/vex/seed-small.sh
t/vex/integration-mysql-minio.sh
t/vex/mysql-s3-stock-http.sh
t/vex/compat-stock-client.sh
scripts/vex/down.sh
```

`git-http` builds this checkout's Git source with `VEX_MYSQL=YesPlease`, then
serves normal smart HTTP through that binary.  Its thin `*.git` directories
carry `extensions.refStorage=vex://<uuid>` discovery configuration only:
metadata and refs use MySQL, while canonical object bytes use the configured
MinIO/S3 bucket.  The gateway initializes `vex-health.git` at startup and its
`/healthz` probe opens that same Vex store before returning
`"vex_storage_active": true`.

`t/vex/mysql-s3-stock-http.sh` creates an empty Vex repository and proves an
unmodified host Git client can push, clone, fetch, and fsck over local HTTPS.
It is the Compose data-plane E2E gate, not a clone-SLO benchmark: the server
still invokes `git-http-backend` per request and has no p50/bitmap-class SLO
evidence yet.

Derived packs are immutable S3 artifacts, never the canonical object-byte
store. Once a receive seals a newer generation, the old pack envelope becomes
`obsolete` and cannot be selected for serving. The same MySQL transaction
records one `pack_rebuild_requests` row with the newest target generation.
Later receives coalesce into that row without moving its original
`not_before`, so normal pushes never synchronously repack or starve a busy
repository's builder. The default batching window is 60 seconds; set
`GIT_VEX_PACK_REBUILD_DELAY_SECONDS` to a bounded nonzero value when starting
the gateway.

The standalone pack-level Vex worker consumes one due normal-path request:

```sh
docker compose -f vex/docker/compose.yaml --env-file vex/docker/.env \
  --profile pack-worker build pack-worker
docker compose -f vex/docker/compose.yaml --env-file vex/docker/.env \
  --profile pack-worker run --rm --no-deps pack-worker --once
```

It builds one derived pack, performs one immutable S3 pack upload and one
index upload (multipart above the configured part size), then bulk-loads only
OID-to-pack-offset metadata into MySQL. It cannot change refs, ref audit rows,
generations, or canonical object bytes. The initial worker intentionally does
not consume the raw receive-pack WAL: that path remains synchronously sealed
by the C receive transaction so refs never outrun durable S3 bytes.

`scripts/vex/build-pack.sh group/repository.git` remains the explicit
immediate builder for maintenance and comparison.

The image keeps the explicitly labelled classic-bare listener as a direct
image fallback when `VEX_GIT_HTTP_COMMAND` is absent. Compose deliberately
sets the bundled Vex command by default, so an empty/false health payload is a
startup failure rather than a misleading protocol-only success. A derived
image may override `VEX_GIT_HTTP_COMMAND` and `VEX_REPO_INIT_COMMAND`, provided
it preserves the MySQL+S3 health contract.
