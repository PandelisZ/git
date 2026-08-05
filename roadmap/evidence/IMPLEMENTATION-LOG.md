# Implementation log

## 2026-08-05 — Wave A3 / early write-path harness

Shipped a stock-Git incremental push-stress harness under `scripts/vex/` and
`t/vex/push-stress/`.

- Deterministic first-parent, topo, explicit, fixed, ramp, seeded-random, and
  list batch planning writes `plan.jsonl`.
- The driver pushes each planned tip with ordinary `git push`; each actual child
  attempt is recorded in `metrics.jsonl`.
- Strict checkpoint/resume validation binds plan SHA-256, source, origin,
  branch, and advertised server tip.
- Final recon makes a fresh target fetch and runs `git fsck --full`; an optional
  server-owned command records canonical-byte reconciliation without the
  harness directly accessing storage.
- Smoke, ramp, steady, chaos, and long-run profiles are committed. The smoke
  profile was run successfully against a temporary classic bare repository.

Commands and result:

```sh
t/vex/push-stress/test-plan-determinism.sh
# PASS: deterministic plan.jsonl

t/vex/push-stress/test-classic-bare.sh
# PASS: 20/20 batch=1 pushes, final fresh-fetch/fsck recon true
```

Current blocker for backend-specific acceptance: the Vex object/metadata and
smart-HTTP implementation is not yet available to this harness. In particular,
this evidence does not claim epoch compensation, canonical blob durability, or
any clone/push SLO.

## 2026-08-05 — Wave A1/A2 and Compose foundation

Implemented the local Vex storage foundation:

- `vex/schema.sql` records metadata and blob keys only; raw Git object and
  pack bytes are outside MySQL in the blob-store access pattern.
- The fs store uses `git/v1/<repo_uuid>/objects/<oid>` keys, validates object
  hashes, stages blobs before metadata, supports epoch compensation, typed
  ref CAS, immutable ref audit rows, and commit-parent metadata.
- Git now recognizes `ODB_SOURCE_VEX`, `REF_STORAGE_FORMAT_VEX`, `vex://%%
  repository identifiers, `GIT_REFERENCE_BACKEND`, `GIT_VEX_FS_ROOT`, and
  `vex.storeRoot`.
- Unsupported maintenance (`gc`, `repack`, MIDX, commit-graph write)
  safe-fails with an operator error rather than dereferencing a file ODB.

Verified locally:

```sh
env -u CARGO_TARGET_DIR make -j4 git
env -u CARGO_TARGET_DIR make -C t unit-tests
t/unit-tests/bin/unit-tests -r vex_store
make -C t T=t1490-vex-storage.sh
make -C t T=t9905-vex-fs-first-write.sh
# PASS: full unit suite 259/259; Vex store group 3/3;
# t1490 2/2; t9905 1/1

scripts/vex/up.sh
t/vex/integration-mysql-minio.sh
scripts/vex/seed-small.sh
t/vex/compat-stock-client.sh
scripts/vex/down.sh
# PASS: MySQL schema and MinIO bucket ready; TLS stock Git bootstrap
# clone/fetch/push passed. /healthz reports vex_storage_active:false.
```

The Compose result is intentionally limited: it proves local dependency and
stock-Git transport wiring, not Vex MySQL/MinIO Git storage. The next
functional gate is an unmodified Git push/fetch/clone through a Vex-aware
receive-pack/upload-pack implementation.

## 2026-08-05 — Vex-FS protocol epochs and live backend drivers

The Vex-FS protocol path is operational with an unmodified `/usr/bin/git`
client:

- `t1491-vex-fs-http.sh` passes stock clone/fetch through the Vex-FS
  smart-HTTP server; the full smoke passes clone/fetch/push.
- `t1492-vex-receive-pack.sh` passes a stock push using the Vex ODB and
  leaves no open receiver epoch.
- `t1493-vex-receive-epoch.sh` passes accepted receive sealing and
  rejected-receive compensation: no rejected metadata row, final blob, or
  open epoch remains.
- `t9905-vex-fs-first-write.sh` passes the first blob/tag write to a
  fresh Vex bare repository.

The HTTP harness now stops cleanly under the in-tree test environment and
unsets the in-tree `GIT_EXEC_PATH` for the stock client, so its remote
HTTP helper is the installed client helper rather than a Vex build artifact.

Two native external drivers also passed against local Compose:

- `meta-mysql` wrote and read metadata only (live result: `tables=10`,
  `ref_version=2`, and `metadata_only=1`; its throwaway generation number
  varies by run).
- `blob-s3` used SigV4 with MinIO to stage, promote, fetch, and
  re-hash Git object bytes under `git/v1/<repo>/objects/<oid>`.

This is deliberately not an L2 or L4 claim. The central `vex_store` is
still FS-backed, and the Compose `git-http` service reports
`mode:"bootstrap-classic-bare"` with `vex_storage_active:false`.
The next integration slice is a shared store backend that makes the same
stock-Git protocol path use the MySQL metadata and S3 blob drivers.

## 2026-08-05 — Vex FS-metadata + S3-byte central-store slice

Shipped the first shared-store vertical slice that selects signed MinIO bytes
without moving raw Git payloads into MySQL:

- `GIT_VEX_BLOB_BACKEND=s3` selects `VEX_STORE_BACKEND_FS_S3`; the existing
  FS backend continues to hold refs, object metadata, and epoch state.
- Blob stage/read/promote/compensate calls dispatch through the selected
  backend. S3 promotion uses the canonical
  `git/v1/<repo-id>/objects/<oid>` key; compensation deletes staged keys only
  and never deletes a potentially shared content-addressed final key.
- The loopback smart-HTTP harness attests both `metadata_backend=fs` and the
  selected blob backend, so its stock-client proof cannot be mislabeled as
  all-FS storage.
- `scripts/vex/print-env.sh` emits the active S3-byte selector and region,
  while documenting that `GIT_VEX_BACKEND=mysql+s3` remains a topology label.

Verified locally:

```sh
env -u CARGO_TARGET_DIR make -j4 git
env -u CARGO_TARGET_DIR make -C t unit-tests
# PASS: 262/262

make -C t T='t1490-vex-storage.sh t1491-vex-fs-http.sh \
  t1492-vex-receive-pack.sh t1493-vex-receive-epoch.sh \
  t9905-vex-fs-first-write.sh'
# PASS: 6/6

VEX_TEST_S3=true make -C t T='t1494-vex-s3-store.sh \
  t1495-vex-s3-http.sh'
# PASS: 2/2

VEX_TEST_S3=true make -C t T=t1496-vex-s3-receive-epoch.sh
# PASS: 1/1
```

The S3 gate proves: exact MinIO final-key placement plus a fresh Vex reader;
unmodified `/usr/bin/git` clone/fetch/push through the loopback Vex HTTP
server; and rejected non-fast-forward receive compensation with no final key,
no staged key, no rejected metadata, no open epoch, and no tip movement.

This is intentionally not L2 or L4: metadata remains FS-backed and the
bundled Compose `git-http` service remains an explicit classic-bare bootstrap.
The next required slice is MySQL metadata in the central store, followed by a
Vex-aware Compose server and the staged push-stress/chaos gates.
