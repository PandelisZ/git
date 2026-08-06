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

## 2026-08-05 — Active Vex MySQL + S3 Compose data plane

Shipped the first active Vex Compose server whose durable metadata and object
bytes follow the intended split:

- `VEX_STORE_BACKEND_MYSQL_S3` maps the existing Vex state machine to the
  normalized MySQL schema while canonical object bytes remain in MinIO/S3.
  MySQL records IDs, metadata, generations, refs, and audit rows; it never
  stores raw Git object payloads.
- `GIT_VEX_META_BACKEND=mysql` and `GIT_VEX_BLOB_BACKEND=s3` select the active
  data plane. `GIT_VEX_BACKEND=mysql+s3` remains a convenient topology alias.
- Compose builds the Vex Git server with `VEX_MYSQL=YesPlease` and runs a
  long-lived smart-HTTP gateway. Its health check opens the same Vex store
  used by protocol requests, so `vex_storage_active:true` requires both
  MySQL metadata and S3 object-byte access.
- The regular build remains supported: switching between `make git` and
  `make VEX_MYSQL=YesPlease git` now rebuilds only `vex/meta-mysql.o`, avoiding
  stale native-client symbols in the normal no-MySQL binary.

Verified locally:

```sh
make -j4 git
make -j4 VEX_MYSQL=YesPlease git
make -j4 git
# PASS: all three links succeed; the MySQL mode switch rebuilds meta-mysql.o

make -C t T=t1490-vex-storage.sh
# PASS: 2/2 (Vex storage and maintenance safe-fail regression)

VEX_COMPOSE_NO_BUILD=1 VEX_TEST_COMPOSE_MYSQL_S3=1 \
  make -C t T=t1497-vex-compose-mysql-s3.sh
# PASS: 1/1 stock /usr/bin/git HTTPS push, clone, fetch, atomic annotated
# tag+branch, branch delete, depth-one clone; MySQL rows + MinIO keys checked
```

The Compose project remains loopback-only and is running as `vex` with
`vex-git-http-1`, `vex-mysql-1`, and `vex-minio-1`. This is a functional L2
vertical slice, not L4: the read path is still loose-object correctness work,
and sealed packs, benchmark numbers, stress/chaos, fault injection, sweeping,
and clone leases remain required before any performance or completion claim.

## 2026-08-05 — MySQL+S3 receive safety and serial smoke

The active Vex Compose path now has the first useful L2 write-boundary gates:

- `t1499-vex-compose-mysql-s3-receive-epoch.sh` passes rejected receive
  compensation. A rejected update leaves no rejected MySQL object/ref state,
  no canonical or staging MinIO object, and no changed `main`; accepted data
  remains readable.
- `t1498-vex-push-stress-compose.sh` passes against a unique empty Vex repo
  with only host `/usr/bin/git` on the wire. It completes 20/20 batch=1
  pushes, reports `failures=0`, matches the planned final tip, and performs a
  fresh fetch plus `git fsck` reconciliation. Latest measured smoke latency:
  p50 `1622.890ms`, p90 `2332.091ms`, p99 `2556.664ms`.
- Concurrent first pushes to different repositories revealed that the old
  globally-unique `ref_updates.id` was incompatible with per-repository row
  locks. The schema now uses a non-unique leading ID index and selects the
  next audit ID per repository; an advisory-lock migration upgrades existing
  local volumes. `t1500-vex-compose-mysql-s3-cross-repo.sh` passes two
  simultaneous stock-Git HTTPS pushes and verifies both MySQL tips plus one
  audit row per repository.

These are correctness and write-path smoke results only. Ramp/chaos/fault
injection, sweep/scrub, sealed packs, and the git/git clone SLO remain open.

## 2026-08-05 — MySQL+S3 safety foundation and 500-commit ramp

Extended the active Compose Vex receiver with measured write-path and
read-safety gates. All use stock host `/usr/bin/git` over HTTPS; MySQL keeps
only metadata and MinIO/S3 remains the canonical object-byte source.

- `t1498-vex-push-stress-ramp-compose.sh` replays 500 synthetic commits into
  a unique empty Vex repository in 20 batches (`1,2,4,8,16,32×14,21`). The
  registered run passed 20/20, fresh-fetch/`fsck`, final-tip match, and
  MySQL/S3 reconciliation. Warm-stack write latency was p50 `13706.237ms`,
  p90 `23252.034ms`, p99 `28033.833ms`.
- `t1501-vex-compose-mysql-s3-promotion-fault.sh` injects one staged-to-final
  S3 promotion failure. It observes an aborted generation and proves no new
  ref, published object, generation claim, audit row, canonical S3 key, or
  staging key before a normal retry clones and fscks successfully.
- `t1502-vex-compose-mysql-s3-scrub.sh` walks every MySQL-indexed published
  object, streams its canonical MinIO/S3 key, and re-hashes the Git object
  header plus content. It also rejects refs without published metadata.
- `t1503-vex-compose-mysql-s3-sweep-plan.sh` adds durable pin/unpin plus a
  conservative read-only graph mark. Its E2E shows three sealed candidates,
  pin protection across a later stock-Git push, and candidates returning after
  unpin. It intentionally performs no deletion.
- `t1518-vex-compose-mysql-s3-concurrent-readers.sh` passes 20 writer pushes
  with 8 concurrent stock-Git readers: 8 clones, 246 fetches, 8 final fscks,
  final-tip agreement, and MySQL/S3 reconciliation.

This is still below L3 and L4: hard-kill chaos, grace-period deletion,
pack/clone-lease roots, sealed-pack serving, `git/git` import, and clone SLOs
remain open.

## 2026-08-05 — S3 immutable pack artifact and measurement harness

Added the first derived-pack artifact slice while keeping raw Git payloads
outside MySQL:

- `scripts/vex/build-pack.sh <repo.git>` uses the source-built server Git to
  make a deterministic single-threaded full pack/index from live refs. It
  streams both artifacts to `git/v1/<repo_uuid>/packs/<pack_id>.{pack,idx}`
  in MinIO/S3 and writes only their keys, covering generation, and object IDs
  to `pack_envelopes` / `pack_objects`.
- The MySQL state-replacement adapter now retains immutable pack rows through
  a normal stock-Git receive, instead of refusing all writes once a pack row
  exists. This is metadata preservation, not pack-currentness or deletion.
- `t1521` proves pack/index S3 byte placement, MySQL object membership, a
  subsequent stock-Git push, fresh clone/`fsck`, and full S3 object scrub.
- `t1519` adds stock-client branch collaboration coverage: rebase,
  exact `--force-with-lease`, and a `--no-ff` integration merge.
- `scripts/vex/bench-clone.sh` and `t1520` emit stock-Git HTTPS-vs-local
  disk p50/p90 JSON. The artifact explicitly marks itself
  `measurement_only:true` and `l4_success_claim:false`.

Verified locally:

```sh
make -j4 VEX_MYSQL=YesPlease git vex-sweep-plan
make -C t unit-tests && t/unit-tests/bin/unit-tests -r vex_store
# PASS: 262/262 full unit tests; Vex store 4/4

VEX_COMPOSE_NO_BUILD=1 VEX_TEST_COMPOSE_MYSQL_S3=1 \
  make -C t T=t1519-vex-compose-mysql-s3-workflows.sh
# PASS: 1/1

VEX_COMPOSE_NO_BUILD=1 VEX_TEST_COMPOSE_MYSQL_S3=1 \
  make -C t T=t1521-vex-compose-mysql-s3-pack-artifact.sh
# PASS: 1/1

VEX_COMPOSE_NO_BUILD=1 VEX_TEST_COMPOSE_MYSQL_S3=1 \
  make -C t T=t1520-vex-compose-clone-benchmark.sh
# PASS: 1/1 measurement artifact shape and stock-client clone/fsck checks
```

This does not satisfy any pack-serving or L4 checkbox. There is no
pack invalidation on tip move, clone lease, local pack cache, bitmap/MIDX
acceleration, direct pack stream, `git/git` fixture, or accepted clone SLO
yet.

## 2026-08-05 — Verified current S3 pack cache

Added a bounded server-side read accelerator while preserving MySQL metadata
and S3 as the durable split:

- The MySQL selector returns only a `sealed` artifact whose
  `covering_generation` exactly equals the newest sealed generation. It also
  validates the schema's padded object-id representation and the cache reader
  independently derives the expected per-repository S3 pack/index keys, so
  metadata cannot redirect a pack download.
- `odb_source_vex` streams the selected pack and index into a named scratch
  volume, verifies the Git index and pack checksum, and exposes it as a
  disposable packed-object source. Any cache miss, invalid artifact, or
  download failure falls through to canonical Vex object reads.
- The global pack bookkeeping recognizes the Vex-owned packed source, keeping
  normal window and file-descriptor eviction safe without treating scratch as
  a source of truth.
- `t1522` first materializes the pair, then restarts only `git-http` with
  `GIT_VEX_TEST_S3_FAIL_READS=1`. The hook rejects every S3 byte GET,
  including derived-pack download; a fresh host stock-Git clone and `fsck`
  still pass from the persisted verified cache.

Verified locally:

```sh
make -j4 VEX_MYSQL=YesPlease git git-http-backend vex-sweep-plan
# PASS

make -C t unit-tests && t/unit-tests/bin/unit-tests -r vex_store
# PASS: 262/262 full unit tests; Vex store 4/4

VEX_COMPOSE_NO_BUILD=1 VEX_TEST_COMPOSE_MYSQL_S3=1 \
  make -C t T=t1522-vex-compose-mysql-s3-pack-cache.sh
# PASS: materialized current cache serves stock clone/fsck with all S3 byte reads faulted

VEX_COMPOSE_NO_BUILD=1 VEX_TEST_COMPOSE_MYSQL_S3=1 \
  make -C t T=t1521-vex-compose-mysql-s3-pack-artifact.sh
# PASS: a subsequent receive retains membership and obsoletes the stale artifact

VEX_COMPOSE_NO_BUILD=1 VEX_TEST_COMPOSE_MYSQL_S3=1 \
  make -C t T=t1519-vex-compose-mysql-s3-workflows.sh
# PASS: stock rebase, force-with-lease, and no-ff merge workflow
```

This is not direct precomputed-pack streaming or an L4 performance claim. The
next safety work is grace-period deletion rooted in tips, pins, pack members,
and live clone leases; cache lifecycle reclamation and direct pack serving stay
open.

## 2026-08-05 — Pack and clone-lease sweep roots

Added the Stage 6 retention-root slice without enabling any deletion:

- `clone_leases` stores a random opaque 16-byte lease id, pack id, and bounded
  expiry in MySQL. It has no foreign key to `pack_envelopes`, because normal
  Vex state saves temporarily replace pack rows; raw Git object and pack bytes
  remain in MinIO/S3.
- The read-only sweep plan now marks current sealed-pack members and
  non-expired clone-lease members alongside tips and durable pins. It reports
  separate `pack_roots` and `lease_roots` counts before emitting conservative
  sealed candidates.
- `odb_source_vex` acquires a 900-second clone lease before installing a local
  verified packed cache and attempts release when the source closes. The TTL
  is also the safe fallback for short-lived CGI processes or crashes. Test-only
  controls shorten automatic CGI leases and disable the derived cache in the
  planner process so root behaviour is observable; neither changes the
  production default.
- `vex-sweep-plan` adds `--lease-current-pack=<seconds>` and
  `--release-pack-lease=<id>` for the operator/test path.

Verified locally:

```sh
make -j4 VEX_MYSQL=YesPlease git git-http-backend vex-sweep-plan
# PASS

cd t && VEX_TEST_COMPOSE_MYSQL_S3=1 \
  ./t1503-vex-compose-mysql-s3-sweep-plan.sh
# PASS: 1/1

cd t && VEX_TEST_COMPOSE_MYSQL_S3=1 \
  ./t1521-vex-compose-mysql-s3-pack-artifact.sh
# PASS: 1/1

cd t && VEX_TEST_COMPOSE_MYSQL_S3=1 \
  ./t1522-vex-compose-mysql-s3-pack-cache.sh
# PASS: 1/1

cd t && VEX_TEST_COMPOSE_MYSQL_S3=1 \
  ./t1523-vex-compose-mysql-s3-pack-lease.sh
# PASS: 1/1
```

`t1523` builds a three-object current pack, acquires a 900-second lease,
force-updates the tip so that pack becomes obsolete, verifies `lease_roots=3`
and zero candidates, releases the lease, and verifies `lease_roots=0` with
three conservative candidates. It then fresh-clones and fscks with stock Git.
No blob or metadata deletion, direct pack serving, or L4 performance claim is
made.

## 2026-08-06 — Standalone Vex pack-level worker (normal receive path)

Added an opt-in `vex-pack-worker` Compose profile for the durable derived-pack
path. The worker claims exactly one MySQL rebuild row with `FOR UPDATE SKIP
LOCKED`, records a UUID lease and claimed generation, builds one complete
pack/index from the source-built Vex Git binary, uploads immutable pack/index
keys to S3-compatible storage, and streams one trusted OID-to-offset TSV into
a temporary MySQL table for set-based membership and offset insertion.

The C receive path remains the only owner of connectivity checks, hooks, refs,
ref audit rows, sealed generations, and canonical object bytes. Publication
locks the repository and refuses a changed target generation; a later receive
therefore makes an in-flight job stale rather than allowing it to publish an
old view. `pack_object_offsets` is now also preserved through C state rewrites,
so a normal follow-up push cannot orphan the immutable envelope.

Verified locally:

```sh
cargo test --manifest-path vex/pack-worker/Cargo.toml --locked
# PASS: 7/7

cargo clippy --manifest-path vex/pack-worker/Cargo.toml --locked \
  --all-targets --all-features -- -D warnings
# PASS

docker compose -f vex/docker/compose.yaml --env-file vex/docker/.env \
  --profile pack-worker build pack-worker
# PASS

VEX_COMPOSE_NO_BUILD=1 VEX_STOCK_GIT=/usr/bin/git \
  t/vex/mysql-s3-pack-worker.sh
# PASS: 9 objects; one second worker run; S3 pack/index, matching 9/9
# membership/offset rows, direct stock-Git pack relay with pack-objects
# faulted, and offset preservation across a later stock-Git push

cd t && VEX_COMPOSE_NO_BUILD=1 VEX_TEST_COMPOSE_MYSQL_S3=1 \
  VEX_STOCK_GIT=/usr/bin/git ./t1532-vex-compose-mysql-s3-pack-worker.sh
# PASS: 1/1
```

This is a correctness and operational slice, not a small-fixture performance
claim. It intentionally does not consume the raw receive-pack WAL used by the
`git/git` seed: that path already has pack-level C publication and must keep
its synchronous S3-before-ref durability boundary until an explicit raw-pack
handoff is implemented.
