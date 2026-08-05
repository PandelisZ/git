# Stage Playbooks PRD

**Status:** Draft  
**Current Stage:** 0 - Planning  
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
| L2 | Cloud functional | Stage 4 mysql+s3 |
| L3 | Multi-tenant beta | Stage 6 + scrub |
| L4 | **User acceptance** | Stage 7: stock client F1–F8 on mirrored **git/git** + clone \(T_{us}\le 1.5 T_{gh}\) and \(T_{us}\le 1.5 T_{disk}\) — see [005](005-stock-client-verification-and-benchmark.md) |
| L5 | HA hardened | multiplex + long-lived daemon; p95 holds under drill |

---

## Schema playbook (canonical MySQL)

```sql
-- abbreviated; full DDL lands in cloud/schema.sql at Stage 1

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

**Forbidden:** object payload columns in MySQL.

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
- [ ] **push-stress plan.jsonl dry-run** against local bare git (no cloud) to validate harness  

### Downcast inventory (maintenance guards)

Must no-op/error in cloud mode (non-exhaustive starters):

- `builtin/gc.c`  
- `repack.c`, `repack-midx.c`, `repack-geometry.c`  
- `builtin/multi-pack-index.c`  
- `pack-bitmap.c` writers  
- `commit-graph.c` writers  
- `midx.c` writers  

---

## Stage 1 — fs cloud_store

**Exit toward L1 foundation**  

Owned: `cloud/schema.sql`, `store.*`, `meta-fs.c`, `blob-fs.c`

Tasks:

1. Implement meta fs mirror of schema (sqlite or JSON/files — pick **sqlite file per repo** for fidelity).  
2. Blob fs directory tree by key.  
3. API: `put_blob`, `get_blob`, `index_put_quarantine`, `index_publish`, `ref_txn`, `ref_list`.  
4. Tests: ordering, CAS, tenant isolation (two repo_ids).  

Acceptance:

- No payload in sqlite  
- CAS conflict returns typed error  
- Cross-repo oid same bytes different keys  

---

## Stage 2 — git wiring

Owned: `odb/source-cloud.c`, `refs/cloud-backend.c`, setup/build

Tasks:

1. `ODB_SOURCE_CLOUD` + synthetic path `cloud://<repo>`  
2. `REF_STORAGE_FORMAT_CLOUD`  
3. Config: `extensions.objectstorage`, `extensions.refstorage=cloud://<uuid>`, `cloud.*`, env  
4. Bootstrap HEAD → `refs/heads/main` or configured default  
5. Reflog callbacks no-op/audit read  
6. Maintenance guards  
7. Smoke hash-object/update-ref/cat-file/gc  

Acceptance:

- `.git/objects` empty or unused  
- `git gc` exits 0 with message, no downcast  

---

## Stage 3 — protocol (L1)

Owned: receive-pack/upload-pack integration via ODB txn

Tasks:

1. Map receive-pack `ODB_TRANSACTION_RECEIVE` to open generation (`push_id`).  
2. Quarantine visibility for hooks (Pass 25).  
3. Implement `begin_transaction` / commit / `env` on cloud ODB (hard gate).  
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

Tasks:

1. Land `cloud/docker/compose.yaml` (mysql + minio + minio-init + git-http).  
2. Land `scripts/cloud/{up,down,reset,seed-small,print-env}.sh`.  
3. meta-mysql migrations applied by mysql init or migrate job.  
4. blob-s3 against MinIO (`path-style`, custom endpoint).  
5. Wire git-http container to `GIT_CLOUD_BACKEND=mysql+s3`.  
6. pooling notes for short-lived git processes.  
7. `t/cloud/integration-mysql-minio.sh` + stock clone smoke.  
8. Finalize push-stress on Compose ([009](009-incremental-push-stress-harness.md)): smoke + **ramp** + **chaos** + **fault_inject**.  
9. Post-batch recon asserts (W5-3); anti-cheat checkpoint (W5-5).  
10. Compose resource limits for stress profile (W5-10).  
11. CI: up → seed-small → clone smoke → **push-stress smoke** → down -v.  
12. **Sealed packs still optional** at L2 (W5-4).  

Acceptance:

- L2 checklist green **on Compose** (Valkey off).  
- Stock clone smoke.  
- Empty repo + 20× batch=1; ramp 500 commits; chaos no split-brain.  
- Fault inject: tip unchanged; epoch compensated.  
- Reproducible with Docker + git only.

---

## Stage 5 — Valkey + UX

Tasks:

1. Compose profile `cache` adds Valkey.  
2. Tip cache optional  
3. No negative existence cache v1 (Pass 14)  
4. Invalidate on ref txn  
5. gc messaging; `cloud optimize` stub  
6. Test: `docker compose exec valkey valkey-cli FLUSHALL` mid-traffic — tips still correct from MySQL.  

Acceptance: FLUSHALL safe; default profile still passes without Valkey.

---

## Stage 6 — sealer/sweeper/pins (L3 prerequisite)

Tasks:

1. Seal published gens  
2. Mark unreachable past grace_days (default 14)  
3. Delete blob+index **only if** not in tips ∪ pins ∪ **active advertised packs**  
4. Pins  
5. Optional auto-pin previous default-branch tip on force (open product flag)  
6. Test: object only reachable via advertised pack is not swept  

Acceptance: orphans reclaimed; pins held; pack-referenced oids survive.

### Stage 6 add-on — push stress soak

- [ ] `ramp.env` + `chaos.env` green on Compose  
- [ ] Optional `steady-1.env` 5k batch=1 overnight  
- [ ] Metrics summary checked for error rate = 0  

---

## Stage 6b — scrub/walker (L3 gate)

Tasks:

1. Job: tips → commits/trees → blob exists  
2. Report missing blob_key  
3. Sample mode for large repos  

Acceptance: scrub runs clean on test repo; L3 allowed only with scrub+sweeper.

---

## Stage 7 — Bundle URI / pack path + git/git SLO (L4 user acceptance)

**Product acceptance bar** ([005](005-stock-client-verification-and-benchmark.md) + STATUS W4-12).  
Runs on local Compose ([006](006-local-docker-compose-test-environment.md)).

Tasks:

1. Pack spike from Stage 4 continued: sealed packs, MIDX, local mmap cache + stock bitmaps.  
2. Compose `bench`: seed-git-git + disk-control bare (`repack -adf` flags documented).  
3. **Long-lived** git-http only.  
4. Derived immutable packs in MinIO; covering generation in MySQL; **clone leases**.  
5. Invalidate on tip move; debounced/incremental rebuild (no per-push full repack).  
6. p50: stream pack (range/zero-copy); fallback circuit-broken.  
7. Automate F1–F16 (P0+P1, interop, hammer, chaos).  
8. `bench-clone.sh` anti-fraud JSON (raw trials, compose_hash, CV).  
9. Gates: disk; GitHub with `BENCH_INCLUDE_GITHUB=1`; fetch-after-push ≤2× disk.  
10. 50-conn hammer + 7-day soak.  

Acceptance: **STATUS L4 go/no-go checklist (W4-12) fully green**.  

---

## Stage 8 — hardening (PoC wrap / optional L5 notes)

**PoC exit does not require multi-tenant auth or HA.** Stage 8 documents ops hygiene useful even for an experiment.

Tasks:

1. Design doc `Documentation/technical/cloud-storage.adoc`  
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

## Open Questions

- [ ] sqlite-per-repo vs single sqlite for fs backend  
- [ ] Default branch name bootstrap (`main` vs unborn HEAD only)  
