# Vex Git Forge Storage (Cloud-Scale PoC) PRD

**Status:** Active
**Current Stage:** 4 - MySQL metadata + S3 object-byte data plane; L3 safety foundation
**Owner:** Codex
**Created:** 2026-08-05
**Updated:** 2026-08-05
**Posture:** Experiment / proof of concept — **no tenancy or auth product scope**
**Related:** user request; [002](002-comparative-research-digest.md); [003](003-adversarial-review-log.md); [004](004-stage-playbooks.md); Mononoke; jj; git ODB/refs plugins

Stage 0 design is frozen after 45 adversarial passes. Stages 1–4 now have a
targeted implementation foundation: the active Compose path uses MySQL for
metadata and MinIO/S3 for canonical Git object bytes. The 500-commit ramp,
deterministic promotion-fault compensation, byte scrub, durable pins, and a
read-only reachability mark plan are verified. Hard-kill chaos, destructive
grace-period sweeping, pack/lease roots, and the sealed-pack L4 path remain
pending.

**Implementation identity:** this experiment is the **Vex backend**.  “Cloud
storage” describes its MySQL + blob-store architecture; code, extension URIs,
environment variables, scripts, and test paths use the `vex` name.

---

## Overview

### Problem

Git durable state is disk-native (loose/pack objects, file/reftable refs, reachability GC). That is excellent for laptops and painful for multi-tenant forges:

- scale-out fights POSIX gitdirs
- `git gc` rewrites immutable data (bad for blob stores)
- disk/node loss takes repos unless complex replication (Gitaly/Mesh)
- major forges still keep bare repos on **block devices**; S3 is used for LFS/artifacts, not the ODB

### Goals

1. Opt-in **Vex storage mode** in this git tree for **forge bare repos**.
2. Long-term SoT: **MySQL metadata + blob/S3 bytes**.
3. Ephemeral OK: **Valkey/Redis** (cache/sessions), **local disk** (scratch).
4. **Stock git clients only** over normal `https://…` / SSH URLs — **no custom client tooling**.
5. **Full remote workflows**: clone, fetch, push, branch collaboration, tags, rebase+force-with-lease, merge pushes (local commit/rebase/merge use vanilla git after clone).
6. **Performance bar:** cold clone of mirrored [`git/git`](https://github.com/git/git) (~80k+ commits) at **≤ 1.5×** GitHub wall time (within 50%), plus ≤ 1.5× same-stack local disk git control — see [005](005-stock-client-verification-and-benchmark.md).
7. Replace classic GC durability with **quarantine generations/bubbles + sweeper**.
8. Deliver in staged, testable slices behind format extensions.

### Non-goals

- **Not a product forge** — experiment / PoC only
- **Tenancy** (multi-org isolation, tenant admin, quotas, billing)
- **Authn/Authz beyond PoC stubs** (SSO, OAuth, per-user ACLs, permission models). Local Compose may use a single shared basic-auth password solely to exercise `git push`; that is test plumbing, not a security design
- Cloud laptop working copy (not jj WC mode)
- Bonsai canonicalization v1
- Full forge UI / PRs / issue tracker
- Production multi-tenant HA as a PoC exit criterion
- Every porcelain optimized on day one
- S3-as-POSIX
- MySQL as the primary store for arbitrary raw Git object payloads
- Required classic server reflog
- Any **required** custom client binary / remote helper for clone-fetch-push
- Calling L1–L3 “done” without git/git L4 SLOs

### Success Metrics

| ID | Metric | Target |
|----|--------|--------|
| S1 | SoT location | Tips+index MySQL; bytes blob; no `.git/objects` SoT |
| S2 | Stock client | Unmodified `git` against `https://localhost…` (and GH-class URLs); **zero** required custom client tools |
| S3 | Workflows | F1–F8 matrix in [005](005-stock-client-verification-and-benchmark.md): clone/fetch/push/branch/tag/rebase-push/merge-push |
| S4 | Clone SLO | Mirror of `github.com/git/git`: \(T_{us} \le 1.5\,T_{gh}\) and \(T_{us} \le 1.5\,T_{disk}\) (p50, N≥5) |
| S4b | Fetch-after-push SLO | After one small push invalidates tip pack coverage, incremental fetch p50 ≤ **2.0×** disk control (secondary; product rejection mode) |
| S5 | Crash safety | Kill mid-push ⇒ no tip advance; no index without blob; hooks before publish |
| S6 | GC | `git gc`/repack/midx **safe-fail** with operator message; sweeper respects tips/pins/**active packs** |
| S7 | Loss domains | Redis flush or scratch wipe ⇒ still correct |
| S8 | Dev loop | fs backend unit tests without Docker |
| S9 | Local integrate | Docker Compose MySQL + MinIO (+ optional Valkey); no AWS required for tests ([006](006-local-docker-compose-test-environment.md)) |
| S10 | Shareable verify | Fresh checkout: `scripts/vex/up.sh` → stock `git clone https://localhost…` → harnesses |
| S11 | Compat tier | L4 = protocol **P0+P1** (full clone/fetch/push/tags/atomic + **shallow**); P2 partial clone deferred |
| S12 | Incremental push stress | From **empty** repo, replay git/git (or subset) with stock git pushes at tunable batch sizes; smoke/ramp/chaos gates — [009](009-incremental-push-stress-harness.md) |

**Program acceptance = L4** (compat on git/git + both clone SLOs), all driven from **local Compose** (GitHub timing needs network on bench host). L1–L3 are intermediate only.

---

## Current Repo State

### Insertion points

| Surface | Path | Role |
|---------|------|------|
| ODB callbacks | `odb/source.h` | read/write/stream/for_each/tx |
| Inmemory ODB | `odb/source-inmemory.c` | non-files template |
| Ref backend vtable | `refs/refs-internal.h` | `ref_storage_be` |
| Files/reftable refs | `refs/files-backend.c`, `reftable-backend.c` | patterns to mirror |
| Extensions | `setup.c` (`parse_reference_uri`, `handle_extension`) | `vex://payload` |
| Build | `Makefile`, `meson.build` | register new objs |

### Files coupling

~59 `odb_source_files_downcast` sites (gc, repack, midx, bitmap, commit-graph, http, …). Cloud mode needs **guards** and alternate maintenance behavior.

### Research constraints

Apply C1–C10 and M1–M4 from PRD 002.

---

## Product Requirements

### Users And Workflows

| User | Workflow |
|------|----------|
| Forge operator | Provision repo UUID; MySQL/S3/fs config; run sealer/sweeper |
| Git client | **Unmodified** git: `clone/fetch/push/branch/commit/rebase/merge` workflows; unaware of MySQL/S3 |
| CI | stock `actions/checkout`-class clone against our HTTPS endpoint |
| Forge app | tips + `ref_updates` for UI; optional Valkey; smart HTTP front |
| Contributor | fs-backend unit tests offline |

### Functional Requirements

1. **FR1** `ODB_SOURCE_VEX` for hosting read/write paths.
2. **FR2** `refs_be_vex` transactional CAS tips.
3. **FR3** Object index row for every durable object before tip references it.
4. **FR4** Write-once blob bytes.
5. **FR5** Push quarantine generation/bubble; publish on success; drop on abort/TTL.
6. **FR6** `ref_updates` audit on ref txn (not classic reflog requirement).
7. **FR7** Reflog APIs no-op or audit-backed; must not block push.
8. **FR8** Optional Valkey tip/existence cache; invalidate-after-commit.
9. **FR9** Local disk scratch for pack build/spill only.
10. **FR10** `gc`/`repack`/`midx` never files-downcast crash; no-op or Vex optimize.
11. **FR11** Sweeper deletes only non-tipped, non-pinned, sealed-old objects.
12. **FR12** Config via extensions + `vex.*` / env.
13. **FR13** commit_parents extracted on commit write for negotiation/seal walks.
14. **FR14** Repo isolation by `repo_id` in all keys/rows.
15. **FR15** Smart HTTP(S) front so `git clone https://localhost/<repo>.git` works with stock git (protocol v2 preferred).
16. **FR16** No client-side custom remote helper required for core workflows.
17. **FR17** Host full mirror of `git/git` for benchmark; default-clone HEAD matches GitHub.
18. **FR18** L4 pack path: precomputed pack and/or **bundle-uri** + reachability bitmap (or equivalent) so git/git clone meets S4 — naïve per-oid S3 pack-build is **not** an accepted L4 implementation.

### CLI Requirements

| Surface | Vex behavior |
|---------|----------------|
| init/bootstrap | Vex bare repo (stage 2+) |
| upload-pack / receive-pack | primary path |
| gc / repack | no-op or optimize stub |
| reflog | empty or audit view |
| cat-file / rev-list | debug via Vex ODB |

### Data And State

#### Tiering

| Data | Store | Durability |
|------|-------|------------|
| Ref tips, symrefs | MySQL | SoT |
| ref_updates | MySQL | SoT (retention policy) |
| Object index and compact derived encodings | MySQL | SoT for metadata; derived data may accelerate reads |
| commit_parents | MySQL | SoT (rebuildable from blobs, but treated durable) |
| generations/bubbles | MySQL | SoT |
| object_pins | MySQL | SoT |
| Canonical object bytes | Blob/S3 | SoT; bulk data follows the blob-store access pattern |
| Pack envelopes | Blob + MySQL index | SoT (optional accel) |
| Tip/existence cache | Valkey | Ephemeral |
| Push session | Valkey or MySQL | Ephemeral unless needs restart survivability |
| Pack scratch | Local disk | Ephemeral |

#### Reachability roots

- Live: current tips (resolved symrefs)
- Pins: `object_pins` for restore/legal hold
- Default: **audit does not pin forever**
- Quarantine: live until publish or bubble expiry

#### Write order (per receive-pack / push epoch — W5-1)

1. Allocate **push epoch / generation id** (MySQL) for this transaction
2. Blob PUT to **staging key** fenced by epoch (or conditional put)
3. Verify content hash == oid (W4-6)
4. Object index INSERT (quarantine, epoch) pointing at staging
5. commit_parents if commit
6. Hooks run with quarantine visibility
7. SQL txn: ref CAS + ref_updates (only if hooks OK) — **same epoch**
8. **Promote** blob staging → final; on any failure **compensate entire epoch** (drop index rows, abandon staging, tip unchanged)
9. Cache invalidate; mark derived packs non-covering if tips moved
10. Async seal/sweep / debounced pack rebuild

Chaos tests (push-stress fault_inject + kill) must hit gaps between steps and assert: no tip without objects; no published index without blob; epoch fully aborted or fully committed.

---

## Technical Plan

### Owned Paths

```
roadmap/001-006...
Documentation/technical/vex-storage.adoc     # impl stage
vex/
  schema.sql
  store.h store.c
  meta.h meta-fs.c meta-mysql.c
  blob.h blob-fs.c blob-s3.c
  cache.h cache-valkey.c
  generation.c generation.h
  docker/
    compose.yaml
    .env.example
    mysql/init/ minio/ caddy/ git-http/ certs/
scripts/vex/
  up.sh down.sh reset.sh seed-small.sh seed-git-git.sh bench-clone.sh
  push-stress.sh seed-empty-repo.sh
t/vex/push-stress/   # plan/run/verify + profiles
odb/source-vex.c odb/source-vex.h
refs/vex-backend.c
setup.c repository.h refs.c
Makefile meson.build
t/vex/   # stock client + compose integration + perf
```

### Architecture

```
git client
   |
upload-pack / receive-pack
   |
repository
  |             |
  v             v
odb_source_vex   refs_be_vex
  |             |
  +------+------+
         v
    vex_store
    /    |    \
 meta   blob  cache?
 MySQL   S3   Valkey
 (fs)   (fs)
    \
   local scratch
```

### GC model

Generation/bubble states: `open → sealed → sweepable → swept`

- open: push in progress
- sealed: closed to writes; reachability accounted
- sweepable: past grace, unreachable from tips/pins
- swept: blob+index removed/tombstoned

### Pack strategy (cannot skip for prod perf)

1. Per-oid correctness (write path + fallback)
2. Streaming pack from object reads + scratch + **process-local blob LRU** (correctness fallback only; **circuit-break** if p99 exceeds budget)
3. **Derived packs/bundles** = sealed **immutable** artifacts in blobstore (never mutate in place)
4. **MIDX + geometric/incremental** pack sets preferred over single forever-growing pack
5. **Invalidation:** packs carry covering generation / tip epoch; never advertise non-covering or incomplete packs
6. **p50 serve path (L4):** long-lived git-http → select covering **sealed** pack → **range/zero-copy stream** from MinIO or **mmap local pack cache** — **no SQL-per-oid**
7. **Rebuild policy:** debounced/incremental (not full repack on every batch=1 push)
8. **Clone leases:** in-flight upload-pack holds lease on advertised pack set until stream completes (or TTL)
9. **Serve-mode tension (W5-12):** full clone optimizes for fat sealed packs; push-stress optimizes for fast quarantine epochs. PoC accepts debounce lag: recent commits may be served via incremental packgen until sealed pack rebuild; circuit-break slow fallback
10. **Sequencing (W5-4):** push-stress L2/L3 gates must pass on loose/quarantine path **without** requiring sealed packs; packs are an L4 clone accelerator
11. Optional delta manifests later

### Sweeper vs packs

Reachability roots for delete = **tips ∪ pins ∪ objects in active advertised packs ∪ objects covered by live clone leases**.
Never sweep an oid still required to serve a non-invalidated pack. Tombstone + delayed delete window.

### Security / tenancy (PoC posture)

- **Out of scope for the experiment:** real multi-tenant authz, SSO, per-user ACLs, tenant billing
- **In scope only as engineering hygiene (not product):** `repo_id` key prefix so two PoC repos do not clobber bytes; dev secrets stay off the git client; Compose binds localhost
- Push tests: trivial shared credential in Compose `.env` is enough
- Redaction, IAM roles, production threat model → later, outside PoC exit

---

## Implementation Stages

### Stage 0: Discovery and design freeze

**Status:** Complete (design freeze ready; no code)
**Owned Paths:** `roadmap/*.md`

- [x] Inspect ODB/refs plugins
- [x] Mononoke + jj local study
- [x] Web prior-art digest (002)
- [x] Forge framing; no required reflog; tiering decisions
- [x] ≥20 adversarial iterations recorded (003; 45 passes)
- [x] Stage 0 blockers frozen (see Open Questions)
- [x] Playbooks aligned (004)

Run:

```sh
test -f roadmap/001-cloud-scale-git-forge-storage.md
test -f roadmap/002-comparative-research-digest.md
test -f roadmap/003-adversarial-review-log.md
rg -n "odb_source_files_downcast" --type c -g '!t/*' | wc -l
```

Acceptance:

- Constraints applied; blockers resolved or deferred with owner
- Review log shows iterations → plan diffs

### Stage 1: vex_store + fs backend + schema

**Status:** Complete — fs foundation
**Owned Paths:** `vex/schema.sql`, `vex/store.*`, `vex/meta-fs.c`, `vex/blob-fs.c`, tests

- [x] Schema: repos, refs, ref_updates, objects, commit_parents, generations, object_pins
- [x] fs metadata + fs blob store
- [x] blob-then-metadata staging/publish helper
- [x] Unit tests: CAS, ordering, isolation

Run:

```sh
env -u CARGO_TARGET_DIR make -C t unit-tests
t/unit-tests/bin/unit-tests -r vex_store
```

Evidence: the Vex fs unit group passes 4/4 for blob-before-metadata and
repository isolation, stale CAS atomicity, and `commit_parents` metadata.

Acceptance reached for the fs foundation: round-trip without `.git/objects`;
stale CAS fails; no metadata index without a blob.

### Stage 2: odb + refs Vex backends wired

**Status:** Complete — local wiring foundation
**Owned Paths:** `odb/source-vex.c`, `refs/vex-backend.c`, `setup.c`, `repository.h`, `refs.c`, build files

- [x] `ODB_SOURCE_VEX`, `REF_STORAGE_FORMAT_VEX`
- [x] `extensions.refstorage=vex://<repo-id>`, `GIT_REFERENCE_BACKEND`, `GIT_VEX_FS_ROOT`, and `vex.storeRoot`
- [x] metadata-backed ref audit rows
- [x] downcast guards for gc/repack/midx/commit-graph
- [x] smoke: hash-object, update-ref, cat-file

Evidence: `t/t1490-vex-storage.sh` passes 2/2 and
`t/t9905-vex-fs-first-write.sh` passes 1/1. Object bytes land under
the Vex blob root; refs land in Vex metadata; unsupported maintenance commands
safe-fail with an operator message.

Acceptance reached for the local wiring foundation. This is not yet
receive-pack/upload-pack support.

### Stage 3: receive-pack / upload-pack

**Status:** Active — Vex-FS smart-HTTP and accepted/rejected receive epochs
**Owned Paths:** quarantine glue; `t/` tests; Vex txn + local blob cache

- [x] Map `ODB_TRANSACTION_RECEIVE` → open generation (`push_id`)
- [x] `begin_transaction` / commit / `odb_transaction_env` on the Vex ODB, including cross-process `GIT_VEX_EPOCH` handoff
- [~] Quarantine visibility: receive children read the parent epoch; hook-isolation coverage remains
- [x] Publish objects with ref updates on accepted receive; compensate rejected receive objects
- [~] abort rejected receive epochs; TTL expiry remains for the sweeper stage
- [ ] negotiation using `commit_parents` when present
- [x] pack stream via scratch for the Vex-FS protocol smoke
- [ ] process-local blob LRU (+ optional scratch object cache)

Evidence: `t1491-vex-fs-http.sh` passes stock-Git clone/fetch; the full
`scripts/vex/fs-http-smoke.sh` passes stock clone/fetch/push.
`t1492-vex-receive-pack.sh` and `t1493-vex-receive-epoch.sh` pass
accepted/rejected receive epoch handling. Remaining L1 work is hard-kill fault
injection, hook visibility/isolation, multi-ref/concurrent coverage, and the
push-stress harness against this endpoint.

### Stage 4: MySQL + S3 via local Docker Compose (L2)

**Status:** Active — MySQL metadata + S3-byte Compose data plane
**Owned Paths:** `meta-mysql.c`, `blob-s3.c`, `vex/docker/compose.yaml`, `scripts/vex/*`, `t/vex/integration-mysql-minio.sh`

- [x] Compose: mysql + minio + git-http (ports on 127.0.0.1)
- [x] MinIO bucket init; Vex schema init
- [x] TLS stock-Git bootstrap clone/fetch/push smoke
- [x] Native MySQL metadata probe (metadata rows only; no raw Git bytes)
- [x] Signed S3/MinIO blob probe (stage, promote, read, Git hash verification)
- [x] Select signed S3 canonical bytes from `vex_store` with `GIT_VEX_BLOB_BACKEND=s3` while retaining FS metadata
- [x] Integrate the MySQL metadata driver into `vex_store` (normalized SQL rows; no Git payload bytes)
- [x] Wire a Vex-aware Compose git-http command to the MySQL+S3 store
- [x] Protocol tests against a Vex `https://localhost…` endpoint: stock push/clone/fetch, atomic tag+branch, branch delete, and depth-one clone
- [x] Empty-repo 20× batch=1 stock-Git HTTPS push stress with fresh-fetch/`fsck` reconciliation
- [x] 500-commit stock-Git ramp (`1,2,4,...,32`) with fresh-fetch/`fsck` and MySQL/S3 reconciliation
- [x] Rejected receive compensates MySQL metadata, canonical/staging S3 keys, and leaves the accepted tip readable
- [x] Deterministic staged-to-final S3 promotion fault compensates its failed receive epoch before a successful retry
- [x] Concurrent first pushes to two repositories retain independent MySQL ref-audit sequences
- [x] S3 byte scrub, durable pin/mark plan, and bounded eight-reader stock-Git correctness gate
- [ ] CI: Compose → Vex seed → Vex smoke → down -v (no AWS)

The Compose default is now a source-built Vex Git image and a long-lived
smart-HTTP gateway. Its health check opens the same Vex store used to serve
clients and reports `metadata_backend:mysql`, `blob_backend:s3`, and
`vex_storage_active:true` only when both are usable. `t1497` uses an
unmodified host Git over HTTPS to verify the basic P0/P1 surface and queries
MySQL plus MinIO afterward to prove the durable split. The old classic-bare
bootstrap remains only an explicitly labelled fallback, not the Compose
default. The serial smoke, 500-commit ramp, deterministic promotion fault,
byte scrub, pin/mark, and bounded-reader gates are green. This does not claim
hard-kill chaos, destructive sweeping, sealed-pack clone performance, or an
L3/L4 readiness level.

Acceptance remains pending: Stage 3 tests must pass on **local** MySQL+MinIO
through a Vex-aware server; Valkey off; second machine can reproduce from
checkout.

### Stage 5: Valkey + maintenance UX

**Status:** Draft
**Owned Paths:** `cache-valkey.c`, compose profile `cache`, gc/optimize messaging

Acceptance: FLUSHALL safe on compose Valkey; stale cache cannot resurrect tips; default compose profile still passes without Valkey.

### Stage 6: sealer / sweeper / pins

**Status:** Active — durable pins and read-only reachability mark
**Owned Paths:** `vex/store.[ch]`, `vex/meta-mysql.[ch]`, operator helper,
Compose tests

- [x] Durable MySQL pins survive later MySQL state writes.
- [x] Read-only graph mark walks refs and pins through canonical S3 bytes.
- [ ] Grace-period deletion of blob+index only after tip/pin/**pack/lease** roots.

Acceptance: **not reached**. Pin protection and conservative candidates are
proven; destructive grace-period sweeping and pack/lease roots are pending.

### Stage 7: Bundle URI / pack path + git/git SLO (L4) — **acceptance stage**

**Status:** Draft
**Owned Paths:** pack/bundle builder; compose profile `bench`; `scripts/vex/seed-git-git.sh`, `bench-clone.sh`; `t/vex/*`

- [ ] Compose bench profile: seed git/git into mysql+minio; disk-control bare on same HTTPS front
- [ ] Stock `git clone https://localhost:8443/git/git.git`
- [ ] Precomputed pack and/or bundle-uri artifacts in **MinIO**
- [ ] Reachability bitmap or equivalent
- [ ] Warm cache path inside git-http container
- [ ] F1–F8 via [005](005-stock-client-verification-and-benchmark.md) on localhost
- [ ] `bench-clone.sh` JSON: \(T_{us}\le 1.5\,T_{disk}\) always; \(T_{us}\le 1.5\,T_{gh}\) with `BENCH_INCLUDE_GITHUB=1`

Acceptance: **S2+S3+S4+S9+S10 green**. User-facing “done” bar; fully local-reproduce except optional GitHub timing.

### Stage 8: hardening + docs

**Status:** Draft
**Owned Paths:** `Documentation/technical/vex-storage.adoc`

Acceptance: threat model + runbook; critical review items closed.

---

## Verification Plan

### Automated

- fs unit tests (no Docker)
- protocol smoke; downcast guards
- **Docker Compose** mysql + MinIO integration + stock `git clone https://localhost…` (CI, no AWS)
- **push-stress smoke**: empty repo, 20× batch=1 from fixture ([009](009-incremental-push-stress-harness.md))
- bench disk-gate on Compose; GitHub-gate on release bench host (`BENCH_INCLUDE_GITHUB=1`)

### Manual / local shareable

```sh
scripts/vex/up.sh
scripts/vex/seed-git-git.sh
t/vex/compat-stock-client.sh
scripts/vex/bench-clone.sh
```

- kill -9 during push; Valkey FLUSHALL; wipe scratch mid-clone

### Rollback

- opt-in per repo; classic unaffected; MySQL backup + S3 versioning (prod)
- dev: `scripts/vex/reset.sh` / `docker compose down -v`

---

## Evidence Log

### 2026-08-05 - Draft / Stage 0 freeze

- Repo ODB/refs + `odb/transaction.h` quarantine; mononoke ephemeral/bookmarks/walker; jj backend; research 002; adversarial 003 (45 passes); playbooks 004.
- Commands: `rg odb_source_files_downcast`; header reads; web research (GitLab #479, Gitea, gitgres, JGit DFS, Mesh, GitHub packs, Piper, bundle-uri, Gitaly quarantine).
- Gaps closed at Stage 0: reflog, tiering, grace, dogfood levels, bundle-uri priority, per-repo keys, txn API.
- Gaps remaining for impl: MySQL/S3 client libs; fs meta sqlite-vs-files; daemon RPC shape.

### 2026-08-05 - Stage 4 MySQL + S3 data plane

- `make VEX_MYSQL=YesPlease git` builds the Vex store with its MySQL metadata adapter; the normal build retains an explicit unavailable-stub error when MySQL support is omitted.
- `scripts/vex/up.sh` starts loopback-only Compose services `vex-mysql-1`, `vex-minio-1`, and `vex-git-http-1`. The server is source-built and the health endpoint reports active MySQL metadata plus S3 bytes.
- `VEX_COMPOSE_NO_BUILD=1 VEX_TEST_COMPOSE_MYSQL_S3=1 make -C t T=t1497-vex-compose-mysql-s3.sh` passed stock Git HTTPS push, clone, fetch, atomic annotated tag plus branch, deletion, depth-one clone, and MySQL/MinIO postconditions.
- No L4 claim: object reads are still the loose-object correctness path; sealed packs, clone SLO measurements, hard-kill chaos, destructive sweeping, and pack/lease roots remain future gates.

### 2026-08-05 - Stage 4 MySQL + S3 receive safety and smoke stress

- `t1499-vex-compose-mysql-s3-receive-epoch.sh` passes the rejected receive
  compensation gate: the rejected tip never appears, its MySQL/S3 state and
  staging keys are absent, and the accepted tip remains readable.
- `t1498-vex-push-stress-compose.sh` passes an empty-repository, stock-Git
  20× batch=1 push run with fresh-fetch/`fsck` reconciliation. The most recent
  smoke sample measured p50 `1622.890ms`, p90 `2332.091ms`, and p99
  `2556.664ms`; these are loose-object write-path observations, not clone SLOs.
- A cross-repository race exposed the old schema's global `ref_updates.id`
  uniqueness under independent repo locks. Audit IDs are now repository-scoped;
  `t1500-vex-compose-mysql-s3-cross-repo.sh` concurrently pushes two fresh
  stock-Git HTTPS repositories and verifies both tips plus one audit row each.
- Existing local volumes migrate `ref_updates_id` to a non-unique leading index
  under a MySQL advisory schema lock. Raw Git bytes remain S3-only.

### 2026-08-05 - MySQL + S3 safety foundation and 500-commit ramp

- `t1498-vex-push-stress-ramp-compose.sh` passed a unique empty-repository,
  stock-Git 500-commit `1,2,4,...,32` replay: 20/20 pushes, final-tip match,
  fresh-fetch/`fsck`, and MySQL/S3 reconciliation. Its registered warm-stack
  write measurement was p50 `13706.237ms`, p90 `23252.034ms`, p99
  `28033.833ms`.
- `t1501` forces one staged-to-final S3 promotion failure and proves an
  aborted generation with no new ref/object/claim/audit/final-or-staging
  bytes before a normal retry clones and fscks.
- `t1502` walks MySQL-indexed published objects, streams canonical MinIO/S3
  bytes, and re-hashes the Git object header plus content. `t1503` proves a
  durable pin protects an old sealed graph across a later push, then unpin
  restores conservative candidates; it intentionally performs no deletion.
- `t1518` passed 20 writer pushes with 8 concurrent stock-Git readers: 8
  clones, 246 fetches, 8 final fscks, final-tip agreement, and MySQL/S3
  reconciliation.

These are L3 foundations only. Hard-kill chaos, grace-period deletion,
pack/clone-lease roots, sealed-pack serving, `git/git` import, and clone SLOs
remain open.

---

## Open Questions

### Stage 0 blockers

- [x] **Repo ID**: forge-assigned UUID in `vex://` payload / config.
- [x] **HEAD symref**: normal refs row; bootstrap on create.
- [x] **Push session durability**: generation rows in MySQL are authoritative; Valkey optional.
- [x] **Grace default**: 14 days before orphan sweep (overridable).
- [x] **Dogfood gate**: private forge ≥ **L3**; perf claims ≥ **L4**.

### Deferred

- [ ] Redaction
- [ ] Partial clone matrix
- [ ] Auto-pin previous default-branch tip on force (product flag)
- [ ] Blob multiplex / cross-region
- [ ] Long-lived daemon RPC surface (design in Stage 8, may split PRD)

---

## Residual risks (post wave-3 panel, 70 passes)

| Risk | Severity | Mitigation stage |
|------|----------|------------------|
| Dual-write split brain | Critical | staging+saga+chaos (3–4) |
| Pack stream vs invalidate race | Critical | sealed packs + clone leases (6–7) |
| Derived pack stale after push | Critical | invalidation + fetch-after-push SLO (7) |
| SQL-per-oid on clone path | Critical | mmap pack cache + bitmap (7) |
| Sweeper vs pack/lease race | Critical | roots include packs+leases (6) |
| CGI/mysql connection stampede | Critical | long-lived git-http before dogfood (4–7) |
| Missing `odb_transaction` / hook order | Critical | Stage 2–3 hard gate |
| Pathological pack DoS | High | size/depth limits + isolation (3) |
| Thin gitdir / http-backend discovery | High | markers (2) |
| Silent maintenance corruption | High | hard-fail + experimental label (2) |
| Per-push full repack cost | High | debounce/incremental (7) |
| Shallow / interop version skew | High | P1 + git 2.39–2.45 matrix (5/7) |
| Bench methodology fraud | High | schema/CV/raw trials (7) |
| Backup/restore ordering | High | runbooks (8) |
| S3 request economics | High | pack-only p50 + cost SLIs (7–8) |
| Compose ≠ prod | High | soak + multi-conn tests (7–8) |
| Operator escape hatch | Med | export-to-bare (8) |
| Gitaly cheaper on hot disk (A8) | Strategic | accept; optional hot tier later |
| Upstream-in-distro pressure (A2) | Strategic | stay experimental until downcast debt gone |

**Largest remaining design hole (panel consensus):** derived-pack lifecycle (build → advertise → invalidate → rebuild) + long-lived serve path. Spike during Stage 4, not after.

---

## Decision Record

1. Forge bare repos first
2. Native Git objects v1 (**GO** vs Bonsai)
3. SoT = MySQL + blob
4. Valkey non-SoT; **no negative oid cache v1**
5. Disk scratch OK + **process-local blob LRU/scratch cache**
6. No required server reflog; `ref_updates` with actor/push_id/reason
7. Blob then meta
8. Bubble/generational GC; mark-from-tips; grace 14d
9. **Bundle URI-first** acceleration; proprietary envelopes secondary
10. Opt-in extensions
11. commit_parents at write time
12. Explicit pins for restore; audit ≠ infinite pin
13. **Per-repo blob keys** (no global dedupe v1)
14. **No alternates v1**
15. Cloud ODB must implement **odb_transaction** (+ env for children)
16. Read path: published globally; quarantine only in owning txn/generation
17. Readiness levels L0–L5; safety dogfood L3+; **user acceptance L4 (git/git ≤1.5× GitHub + disk control)**
18. Stock git client only; smart HTTPS; no required custom client tooling
19. L4 requires precomputed pack/bundle + bitmap-class accel — not per-oid S3 packgen
20. **Tests/verification/benchmarks are local-first via Docker Compose** (MySQL + MinIO S3 + optional Valkey); production AWS optional later
21. **Packs/bundles are derived caches**, not a second SoT; invalidated with tips; sweeper cannot drop pack-referenced objects
22. **p50 clone** streams precomputed pack via long-lived git-http (+ local mmap pack cache / stock bitmap code)
23. **L4 compat = P0+P1** (includes shallow); partial clone P2 deferred
24. **Fetch-after-push** secondary SLO (≤2× disk)
25. **Pack rebuild debounced/incremental**, not per-push full repack of git/git
26. **Export-to-classic-bare** escape hatch before multi-tenant prod
27. Experimental posture: gc/repack/midx safe-fail with clear operator guidance
28. **Not distro-default / not “upstream ready”** until files-downcast debt cleared and dual-write chaos green (A2 dissent)
28b. **PoC experiment only** — no tenancy/auth product; trivial compose auth for push tests only
29. Dual-write **staging + compensate** with mid-push chaos tests
30. **Clone leases** + sealed immutable packs + MIDX/incremental
31. Pack unpack **size/depth limits** + hash verify always
32. Bench fraud controls (raw trials, CV, compose_hash, seed manifest)
33. Dogfood: concurrent hammer + multi-day soak before L4 banner
34. Cost SLIs (S3 GET/clone, bytes, mysql size); pack-centric economics
35. **Push epoch fencing** + whole-epoch compensate (W5-1)
36. Push-stress fault inject + post-batch recon (W5-2, W5-3)
37. Land push-stress smoke as soon as receive-pack works (W5-14)
38. L2/L3 push correctness **before** sealed-pack dependency (W5-4)
