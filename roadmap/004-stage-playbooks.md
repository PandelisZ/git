# Vex Stage Playbooks PRD

**Status:** Active
**Current Stage:** 4 + Stage 6 destructive sweep + Stage 7 direct-pack relay
**Owner:** Codex
**Created:** 2026-08-05
**Updated:** 2026-08-05
**Related:** [001](001-cloud-scale-git-forge-storage.md), [003](003-adversarial-review-log.md), [005](005-stock-client-verification-and-benchmark.md)

---

## Overview

### Posture

**Experiment / PoC.** No tenancy product, no real authz product. Stages prove storage + stock git protocol + local Compose + technical SLOs.

### Problem

PRD 001 stages need agent-executable playbooks: schemas, file lists, tests, and **gates** tied to readiness levels L0–L5.

### Goals

- One playbook per implementation stage
- Include adversarial deltas (quarantine mapping, per-repo keys, scrub, bundle URI)
- Make “done” unambiguous

---

## Readiness levels (from review pass 35)

| Level | Name | Gate |
|-------|------|------|
| L0 | Design | Roadmap freeze |
| L1 | Dev fs correctness | Stage 3 fs push/fetch |
| L2 | Vex functional | Stage 4 mysql+s3 |
| L3 | PoC safety / dogfood | Stage 6 + scrub |
| L4 | **User acceptance** | Stage 7: stock client F1–F8 on mirrored **git/git** + clone \(T_{us}\le 1.5 T_{gh}\) and \(T_{us}\le 1.5 T_{disk}\) — see [005](005-stock-client-verification-and-benchmark.md) |
| L5 | HA hardened | multiplex + long-lived daemon; p95 holds under drill |

---

## Schema playbook (canonical MySQL)

```sql
-- abbreviated; full DDL lands in vex/schema.sql at Stage 1

repositories (
  repo_id BINARY(16) PK,
  name VARCHAR(255) UNIQUE,
  hash_algo VARCHAR(16),
  created_at, deleted_at NULL
)

refs (
  repo_id, refname,
  is_symbolic BOOL,
  target_oid BINARY(32) NULL,
  symref_target VARBINARY(1024) NULL,
  peeled_oid BINARY(32) NULL,
  version BIGINT,
  PRIMARY KEY (repo_id, refname)
)

ref_updates (
  repo_id, id BIGINT,
  refname,
  old_oid, new_oid,
  old_target, new_target,
  is_ff BOOL NULL,
  actor VARBINARY(255) NULL,
  push_id BINARY(16) NULL,
  reason VARCHAR(32),
  message VARBINARY(1024) NULL,
  ts TIMESTAMP(6),
  PRIMARY KEY (repo_id, id)
)

objects (
  repo_id, oid BINARY(32),
  obj_type TINYINT,
  size BIGINT,
  blob_key VARBINARY(512),  -- git/v1/<repo_id_hex>/objects/<oid_hex>
  generation BIGINT,
  state ENUM('quarantine','published'),
  created_at,
  PRIMARY KEY (repo_id, oid)
)

commit_parents (
  repo_id, commit_oid, parent_pos, parent_oid,
  PRIMARY KEY (repo_id, commit_oid, parent_pos)
)

generations (
  repo_id, generation,
  state ENUM('open','sealed','sweepable','swept'),
  push_id BINARY(16) NULL,
  created_at, expires_at NULL, sealed_at NULL
)

object_pins (
  repo_id, oid, reason, expires_at NULL,
  PRIMARY KEY (repo_id, oid, reason)
)
```

**Boundary:** do not make MySQL the primary store for arbitrary raw Git object
payloads. Compact, performance-oriented derived encodings (for example graph,
reachability, pack, or lookup data) may live in MySQL when they do not replace
the canonical blob-store object bytes.

**Blob key layout:** `git/v1/<repo_uuid_hex>/objects/<oid_hex>` (per-repo, Pass 13).

---

## Stage 0 — Design freeze

**Exit:** L0

- [x] PRDs 001–008 exist
- [x] ≥20 adversarial passes (**90** with grok-4.5 panel)
- [x] Freeze: grace_days=14, repo_id=forge UUID, HEAD bootstrap row
- [x] Downcast inventory attached below accepted
- [x] odb_transaction hard requirement noted
- [x] Dogfood L3 / perf L4 gates frozen
- [x] Wave 4 deltas W4-1…W4-12 accepted

### Week 1–2 spikes (before expanding Stage 2) — W4-11 / W5

- [ ] Crash/kill between blob staging and ref CAS (dual-write / epoch)
- [ ] Naive pack+bitmap on ≥10k objects: size, rebuild time (info only; not L2 gate)
- [ ] Baseline `git clone` of disk git/git on bench hardware
- [ ] Failure-injection skeleton
- [ ] **push-stress plan.jsonl dry-run** against local bare git (no Vex backend) to validate harness

### Downcast inventory (maintenance guards)

Must no-op/error in Vex mode (non-exhaustive starters):

- `builtin/gc.c`
- `repack.c`, `repack-midx.c`, `repack-geometry.c`
- `builtin/multi-pack-index.c`
- `pack-bitmap.c` writers
- `commit-graph.c` writers
- `midx.c` writers

---

## Stage 1 — fs vex_store

**Exit toward L1 foundation**

Owned: `vex/schema.sql`, `store.*`, `meta-fs.c`, `blob-fs.c`

Tasks:

1. [x] Implement the fs metadata mirror of the schema (metadata-only state per repo).
2. [x] Implement the blob fs directory tree by key.
3. [x] Implement `put_blob`, `get_blob`, quarantine/publish, typed ref transactions, and ref iteration.
4. [x] Test ordering, CAS, and repository isolation.

Acceptance:

- No payload in metadata state
- CAS conflict returns typed error
- Cross-repo oid same bytes different keys

Evidence: `u-vex-store` passes 3/3, including blob-before-metadata and stale
CAS atomicity.

---

## Stage 2 — git wiring

Owned: `odb/source-vex.c`, `refs/vex-backend.c`, setup/build

Tasks:

1. [x] `ODB_SOURCE_VEX` + synthetic path `vex://<repo>`
2. [x] `REF_STORAGE_FORMAT_VEX`
3. [x] Config: `extensions.refstorage=vex://<uuid>`, `vex.storeRoot`, `GIT_REFERENCE_BACKEND`, and `GIT_VEX_FS_ROOT`
4. [x] Preserve normal symbolic `HEAD` setup while backing refs with Vex.
5. [x] Store immutable ref audit rows.
6. [x] Add maintenance guards.
7. [x] Smoke hash-object/update-ref/cat-file and maintenance safe-fail.

Acceptance:

- `.git/objects` empty or unused
- `git gc` safe-fails with an operator message, no downcast

Evidence: `t1490-vex-storage.sh` passes 2/2 and
`t9905-vex-fs-first-write.sh` passes 1/1. This is a local object/ref
foundation, not an L1 protocol result.

---

## Stage 3 — protocol (L1)

Owned: receive-pack/upload-pack integration via ODB txn

**Implementation snapshot (2026-08-05):** the Vex-FS smart-HTTP server passes
stock Git clone/fetch/push. Receive-pack exports a Vex epoch to child
processes; accepted receives seal it with refs and rejected receives
compensate it. `t1491`, `t1492`, and `t1493` are green.
Hard-kill, hook-isolation, multi-ref/concurrent, and Vex-target push-stress
coverage remain before an L1 claim.

Tasks:

1. Map receive-pack `ODB_TRANSACTION_RECEIVE` to open generation (`push_id`).
2. Quarantine visibility for hooks (Pass 25).
3. Implement `begin_transaction` / commit / `env` on Vex ODB (hard gate).
4. **Staging blob keys + compensate** on failure (W4-1).
5. **Hash-verify** oid on write (W4-6).
6. **Pack size/depth/tree limits** on receive (W4-5).
7. On success: publish objects + **atomic** multi-ref txn after hooks.
8. On fail/TTL: generation expires; staging GC.
9. Generation-scoped reads (no cross-push quarantine leak).
10. upload-pack: graph via commit_parents when possible.
11. Fallback pack stream via scratch + LRU (not p50 final).
12. Tests: hook cat-file quarantine; **F16 chaos mid-push** multi-offset kills.
13. **Land push-stress harness** smoke against fs/HTTP if available (W5-14); else Stage 4.
14. **Push epoch** id on each receive-pack (W5-1).

Acceptance:

- push/fetch/clone works fs backend
- kill -9 at multiple offsets: tips unchanged; no published meta without blob
- hook can read quarantine; second concurrent client cannot
- smoke push-stress 20× batch=1 if HTTP front ready
- **Claim only L1**, not perf; **no sealed-pack requirement**

---

## Stage 4 — MySQL + S3 via Docker Compose (L2)

**Local-only dependencies:** Docker Compose stack in [006](006-local-docker-compose-test-environment.md).

**Implementation snapshot (2026-08-05):** the source-built Compose Git image
now selects `GIT_VEX_META_BACKEND=mysql` and `GIT_VEX_BLOB_BACKEND=s3`.
Its long-lived gateway verifies the active store before reporting healthy.
`t1497` proves a stock host Git can push, clone, fetch, atomically publish a
tag plus branch, delete a branch, and shallow-clone through that gateway; it
then checks MySQL metadata and MinIO keys. This is an L2 vertical data-plane
milestone, not an L4 pack/benchmark result. `t1498` adds 20 stock-Git
batch=1 pushes and a 500-commit `1,2,4,...,32` ramp from empty repositories
with fresh reconciliation; `t1499` proves rejected-receive compensation;
`t1500` covers simultaneous first pushes to separate repositories after the
MySQL audit-sequence migration; `t1501` forces promotion compensation;
`t1502` hashes S3 bytes; `t1503` proves pins and the conservative mark plan;
and `t1518` is a bounded eight-reader stock-Git gate. `t1524` then kills the
real receive worker at `before-ref-commit` and `after-blob-promote`; a
one-second configured recovery restart proves the stale epoch becomes
aborted, MySQL ref/audit/object rows and both MinIO namespaces return to their
baseline, and the identical stock-Git commit retries, clones, and fscks.

Tasks:

1. Land `vex/docker/compose.yaml` (mysql + minio + minio-init + git-http).
2. Land `scripts/vex/{up,down,reset,seed-small,print-env}.sh`.
3. meta-mysql migrations applied by mysql init or migrate job.
4. [x] blob-s3 against MinIO (`path-style`, custom endpoint) for canonical bytes; FS metadata remains for this slice.
5. [x] Wire git-http container to the active MySQL+S3 store. `GIT_VEX_BACKEND=mysql+s3` is a topology alias; the explicit MySQL and S3 selectors activate it.
6. [x] Use a long-lived gateway process; Git protocol children remain bounded request work, so this does not yet satisfy the no-CGI-stampede L4 load gate.
7. [x] `t/vex/integration-mysql-minio.sh` + `t1497` stock-client smoke.
8. [x] Compose smoke push-stress ([009](009-incremental-push-stress-harness.md)): empty repo, 20× batch=1 with stock Git.
9. [x] Post-batch fresh-fetch/`fsck` recon asserts and checkpoint/metrics anti-cheat fields.
10. [x] Compose ramp + bounded crash/fault gates. The 500-commit ramp,
    deterministic S3-promotion fault, and `t1524` two-offset receive-worker
    SIGKILL recovery are green. Wiring hard-kill into a long/soak stress
    profile remains separate work.
11. Compose resource limits for stress profile (W5-10).
12. CI: up → seed-small → clone smoke → **push-stress smoke** → down -v.
13. **Sealed packs still optional** at L2 (W5-4).

Acceptance:

- L2 checklist green **on Compose** (Valkey off).
- Stock clone smoke.
- Empty repo + 20× batch=1; ramp 500 commits; chaos no split-brain.
- 500-commit ramp and deterministic promotion fault: tips reconcile; the
  failed epoch is compensated.
- [x] Two hard-kill offsets leave no ref/audit advance or durable MySQL/S3
  residue after bounded restart recovery.
- Reproducible with Docker + git only.

---

## Stage 5 — Valkey + UX

Tasks:

1. Compose profile `cache` adds Valkey.
2. Tip cache optional
3. No negative existence cache v1 (Pass 14)
4. Invalidate on ref txn
5. gc messaging; `vex optimize` stub
6. Test: `docker compose exec valkey valkey-cli FLUSHALL` mid-traffic — tips still correct from MySQL.

Acceptance: FLUSHALL safe; default profile still passes without Valkey.

---

## Stage 6 — sealer/sweeper/pins (L3 prerequisite)

Tasks:

1. [x] Seal published generations
2. [x] Read-only graph mark from tips, durable pins, current sealed packs,
   and non-expired clone leases
3. [x] Delete blob+index **only if** not in tips ∪ pins ∪ **active advertised
   packs** ∪ **live clone leases**, after a tombstone grace and a second root
   check; final-S3 cleanup retries safely
4. [x] Durable pins
5. Optional auto-pin previous default-branch tip on force (open product flag)
6. [x] Test: a pack becomes obsolete after a stock-Git state rewrite, but its
   members remain roots while a clone lease is live and return as candidates
   after release

Acceptance: `t1528` proves pin protection, grace tombstones, a forced final
S3 delete failure/retry, and a current stock clone/`fsck`. Broader L3 soak is
still distinct from Stage 7 L4 acceptance.

### Stage 6 add-on — push stress soak

- [~] 500-commit ramp and two-offset receive-worker hard-kill recovery green
  on Compose; an integrated long/soak chaos profile remains
- [ ] Optional `steady-1.env` 5k batch=1 overnight
- [ ] Metrics summary checked for error rate = 0

---

## Stage 6b — scrub/walker (L3 gate)

Tasks:

1. [x] Job: published-object index → canonical S3 bytes → Git object hash
2. [x] Report a ref/object or byte/hash mismatch
3. [ ] Sample mode for large repos

Acceptance: scrub runs clean on a stock-Git test repo; L3 remains blocked on
the destructive pack/lease-aware sweeper.

---

## Stage 7 — Bundle URI / pack path + git/git SLO (L4 user acceptance)

**Product acceptance bar** ([005](005-stock-client-verification-and-benchmark.md) + STATUS W4-12).
Runs on local Compose ([006](006-local-docker-compose-test-environment.md)).

Tasks:

1. [~] Pack spike: `scripts/vex/build-pack.sh` builds a deterministic custom-Git pack/index, uploads per-repository S3 artifacts, and records a MySQL envelope/object map. `t1526` now directly relays a verified current cache pack for eligible stock v0/v2 full clones; MIDX, lifecycle rebuild, and scale evidence remain open.
2. Compose `bench`: seed-git-git + disk-control bare (`repack -adf` flags documented).
3. **Long-lived** git-http only.
4. Derived immutable packs in MinIO; covering generation in MySQL; **clone leases**.
5. Invalidate on tip move; debounced/incremental rebuild (no per-push full repack).
6. [~] Stream pack: verified cache-pack relay is proven on stock v0/v2 full clones; p50/large-repo proof and fallback circuit-break measurement remain open.
7. Automate F1–F16 (P0+P1, interop, hammer, chaos).
8. `bench-clone.sh` anti-fraud JSON (raw trials, compose_hash, CV).
9. Gates: disk; GitHub with `BENCH_INCLUDE_GITHUB=1`; fetch-after-push ≤2× disk.
10. 50-conn hammer + 7-day soak.

Acceptance: **STATUS L4 go/no-go checklist (W4-12) fully green**.

---

## Stage 8 — hardening (PoC wrap / optional L5 notes)

**PoC exit does not require multi-tenant auth or HA.** Stage 8 documents ops hygiene useful even for an experiment.

Tasks:

1. Design doc `Documentation/technical/vex-storage.adoc`
2. Short threat note for **local PoC only** (localhost binds, no client cloud creds) — full tenancy/auth threat model **out of scope**
3. Long-lived git-http notes (already needed for L4)
4. Repo delete lifecycle (single-tenant PoC is enough)
5. **Backup/restore sketch** for dev volumes (optional)
6. **export-to-classic-bare** escape hatch
7. **Breakglass:** disable sweeper; force pack rebuild
8. Basic metrics for debugging SLOs (optional)
9. Explicit README: experimental, not a hosted forge product
10. Close open questions that block PoC L4 only

---

## Test matrix (summary)

| Test | Stage |
|------|-------|
| unit meta/blob fs (no Docker) | 1 |
| CAS / isolation | 1 |
| cat-file/update-ref | 2 |
| gc guard | 2 |
| clone/fetch/push classic fs | 3 |
| kill mid-push | 3 |
| **compose up mysql+minio** | 4 |
| stock clone smoke via localhost | 4 |
| valkey flush (profile cache) | 5 |
| sweep/pin on compose | 6 |
| scrub on compose | 6b |
| seed git/git on compose bench | 7 |
| stock client F1–F8 on git/git localhost | 7 |
| clone SLO disk gate (compose) | 7 |
| clone SLO GitHub gate (bench host) | 7 |

---

## Evidence Log

### 2026-08-05

- Playbooks derived from 001 stages + 003 passes 02,05,13,14,16,21,25,26,28,35.
- receive-pack quarantine/`odb_transaction` integration requirement confirmed in-tree.
- Compose Stage 4 data plane: source-built Vex Git, MySQL metadata, and MinIO canonical bytes verified by `t1497`; `t1498` is green for the serial 20× smoke plus reconciliation and the 500-commit ramp; `t1499` is green for receive compensation; `t1500` is green for two concurrent repositories; `t1501` forces S3-promotion compensation; `t1502` hashes canonical bytes; `t1503` proves pin/mark protection; `t1518` runs eight readers during twenty writer pushes; `t1519` covers rebase/lease/merge workflows; `t1520` verifies measurement-only p50/p90 benchmark JSON; `t1521` builds/records a per-repository S3 pack artifact that survives a later receive; `t1522` proves a fresh stock clone and `fsck` use the verified current packed cache when every S3 byte read is faulted; `t1523` proves current pack plus live clone-lease members remain read-only sweep roots across a stock-Git state rewrite and return as candidates only after release; and `t1524` proves two actual `SIGKILL` receive-worker offsets recover through the MySQL state lock, remove stale staging/final S3 keys, retain no tip/audit metadata, and permit the exact commit to retry/clone/`fsck`. `t1525` restores stock protocol v2 through the gateway; `t1526` relays a verified current pack for stock v0/v2 full clones while `git-pack-objects` is forbidden; `t1527` streams a >64 MiB stock-Git request; and `t1528` proves tombstoned S3 reclamation with pin protection and retry-safe final-byte cleanup. Cache lifecycle, git/git-scale measurements, and the L4 SLO remain next.

## Open Questions

- [ ] sqlite-per-repo vs single sqlite for fs backend
- [ ] Default branch name bootstrap (`main` vs unborn HEAD only)
