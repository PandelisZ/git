# Comparative Research Digest PRD

**Status:** Draft  
**Current Stage:** 0 - Research  
**Owner:** Codex  
**Created:** 2026-08-05  
**Updated:** 2026-08-05  
**Related:** [001-cloud-scale-git-forge-storage.md](001-cloud-scale-git-forge-storage.md), [003-adversarial-review-log.md](003-adversarial-review-log.md)

---

## Overview

### Problem

Cloud-scale Git hosting attempts repeatedly fail in the same ways. This digest freezes prior-art lessons as **constraints** for PRD 001.

### Goals

- Distill Mononoke, jj, Git core, GitLab/Gitea/Mesh, gitgres, JGit DFS, GitHub pack economics, Piper, S3-git experiments.
- Export must/must-not constraints.
- Remain citation-backed for later agents.

### Non-goals

- Implement code.
- Exhaustive academic survey.

---

## Systems

### 1. Mononoke (Meta / Sapling) — local tree

**Sources:** `eden/mononoke/docs/2.4-storage-architecture.md`, `2.1-bonsai-data-model.md`, `5.1-git-support.md`; `blobstore/ephemeral_blobstore/schemas/sqlite-ephemeral-blobstore.sql`

| Topic | Practice | Lesson |
|-------|----------|--------|
| Split | Immutable blobstore + SQL metadata | **Adopt** |
| Canonical model | Bonsai; Git derived | **Defer Bonsai**; native Git objects v1 |
| Tips | Bookmarks SQL + update log | **Adopt** tips + `ref_updates` |
| Drafts | Ephemeral bubbles w/ TTL | **Adopt** push quarantine |
| Bytes | Write-once blobs; sqlblob not primary | **No large bytes in MySQL** |
| Perf | Derived delta manifests, bundle URI | **Later phases** |
| Composition | Blobstore decorator stack | **After** fs backend works |

Bubble tables (shape): `ephemeral_bubbles(expires_at,…)`, bubble↔changeset maps — expire as a unit, not classic pack rewrite.

### 2. Jujutsu — local `~/w/jj`

**Sources:** `lib/src/backend.rs`, `store.rs`

- Hard `Backend` boundary under higher layers.
- Thin working copy vs durable store.
- Mutable view ≠ object bytes (op log vs our SQL refs/audit).

### 3. Git upstream — this repo

**Sources:** `odb/source.h`, `odb/source-inmemory.c`, `refs/refs-internal.h`, `setup.c`, downcast audit

- Insert cloud as `odb_source` + `ref_storage_be`.
- `extensions.refstorage=cloud://payload` already fits URI parser.
- ~59 `odb_source_files_downcast` sites → explicit cloud guards required.

### 4. GitLab Gitaly + object-storage epic

**Sources:** Gitaly docs; [epic #479](https://gitlab.com/groups/gitlab-org/-/epics/479); ODB maintenance blog

- Production git data still **block storage**.
- Motivation: cost, node/zone failure.
- Lesson: mounting object storage as a disk is not the answer; API must change.
- Maintenance/repack is a dedicated scale problem.

### 5. Gitea / Forgejo

- Bare git **always local FS**.
- S3 for LFS/attachments/packages/archives only.
- Lesson: industry treats ODB and “blob accessories” differently; we are choosing the harder path intentionally.

### 6. gitgres (Postgres)

**Sources:** [gitgres](https://github.com/andrew/gitgres), [Git in Postgres](https://nesbitt.io/2026/02/26/git-in-postgres.html)

- SQL refs/objects work for demos.
- libgit2 custom backend enables push/clone.
- **Reject** storing large object payloads in SQL rows.

### 7. JGit DFS

- Pack-centric `DfsObjDatabase` over abstract storage.
- Compaction operates on pack units.
- Lesson: **pack envelopes** belong in the roadmap even if v1 is per-oid.

### 8. Bitbucket Mesh

- Horizontally scaled **replicated git disks**.
- HA via replication, not S3-SoT.
- Lesson: still need tenancy, placement, and failure domains in our design.

### 9. GitHub — pack internals series

**Source:** [packed object store](https://github.blog/open-source/git/gits-database-internals-i-packed-object-store/)

- Loose objects do not scale.
- Deltas, bitmaps, MIDX dominate real performance.
- Lesson: per-oid S3 GET clone is a correctness stepping stone only.

### 10. Google Piper

**Sources:** CACM monorepo paper; Piper/Bigtable/Spanner public descriptions

- Centralized metadata DB, not gitdirs.
- Strong transactional mutable state.
- Lesson: monorepo-class scale needs rich indexes, not only KV.

### 11. BucketGit / s3-git-server / jgit-aws

- Common pattern: bucket + local cache; weak or missing SQL index.
- Serverless demos struggle on pack CPU and latency.
- Lesson: **never S3 LIST as database**; require object index.

---

## Failure modes checklist

| # | Failure | Mitigation in 001 |
|---|---------|-------------------|
| F1 | S3-as-POSIX | Explicit cloud ODB/refs APIs |
| F2 | Bytes-in-SQL | Blob store for payloads |
| F3 | Classic gc durability | Generations/bubbles + sweeper |
| F4 | No audit | `ref_updates` |
| F5 | Per-oid clone death | Pack envelopes stage |
| F6 | files downcast crashes | Guards + cloud maintenance paths |
| F7 | Redis SoT | Cache-only policy |
| F8 | Server reflog cargo-cult | Not required; audit instead |

---

## Exported constraints

### Must (C1–C10)

1. C1 MySQL (canonical) for tips, object index, audit, generations  
2. C2 Blob store for bytes + pack envelopes  
3. C3 Valkey optional non-SoT  
4. C4 Local disk scratch/cache only  
5. C5 Native Git objects v1  
6. C6 Push quarantine bubbles/generations  
7. C7 No classic gc as durability  
8. C8 Pack/bundle acceleration before perf claims  
9. C9 Opt-in cloud mode for forge bare repos  
10. C10 Downcast/maintenance call-site plan  

### Must not (M1–M4)

1. M1 Required classic server reflog  
2. M2 Bonsai required v1  
3. M3 S3 LIST as index  
4. M4 Redis durability  

---

## Evidence Log

### 2026-08-05

- Local mononoke/jj/git reads; web search/extract on GitLab, Gitea, gitgres, JGit, Mesh, GitHub packs, Piper, S3 git servers.
- Open: pack CPU isolation practices; Bundle URI priority vs envelopes.

## Additional citations (review wave 2)

- Git `odb/transaction.h` / receive-pack quarantine integration (in-tree).  
- Gitaly [object quarantine doc](https://gitlab.com/gitlab-org/gitaly/-/blob/master/doc/object_quarantine.md).  
- Git [bundle-uri](https://git-scm.com/docs/bundle-uri); GitLab Gitaly bundle URI admin docs; [git-bundle-server](https://github.com/git-ecosystem/git-bundle-server).  
- High-latency / many-small-files git performance reports (directional; clone/fetch sensitivity to RTT).  
- Smart HTTP: [gitprotocol-http](https://git-scm.com/docs/gitprotocol-http), upload-pack/receive-pack — required for stock `git clone https://localhost…`.  
- Benchmark fixture: [github.com/git/git](https://github.com/git/git) (~80k+ commits).  
- Local verification stack: Docker Compose + MinIO-as-S3 + MySQL (PRD 006) — tests must not require real AWS.  

## Open Questions

- [x] Bundle URI phase placement → **Stage 7 primary** (envelopes secondary)  
- [ ] Whether to document Gitaly as deployment peer (RPC front) vs in-process only → Stage 8 daemon design  
