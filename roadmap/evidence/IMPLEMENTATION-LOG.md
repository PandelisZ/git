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
