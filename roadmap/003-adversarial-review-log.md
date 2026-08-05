# Adversarial Review Log PRD

**Status:** Draft
**Current Stage:** 0 - Review
**Owner:** Codex (multi-role simulation: architect, red-team, forge-SRE, git-core, perf, security)
**Created:** 2026-08-05
**Updated:** 2026-08-05
**Related:** [001](001-cloud-scale-git-forge-storage.md), [002](002-comparative-research-digest.md), [004](004-stage-playbooks.md)

---

## Overview

### Problem

A storage redesign can look coherent and still be wrong operationally. This log forces **repeated adversarial passes** against PRD 001, each producing a concrete plan change or an explicit risk acceptance.

### Goals

- ≥20 numbered review passes with distinct angles
- Each pass: **Attack → Evidence → Verdict → Plan delta**
- Keep 001/004 updated when deltas land

### Method

Roles rotate: `git-core`, `forge-SRE`, `perf`, `security`, `consistency`, `cost`, `compat`, `ops-jobs`.
Evidence from this repo, Mononoke/jj trees, and web research.

---

## Pass log

### Pass 01 — git-core: files downcast landmines

**Attack:** Cloud ODB lands but `git gc`/`repack`/`midx` call `odb_source_files_downcast` and BUG/crash.
**Evidence:** ~59 downcast sites (`builtin/gc.c`, `repack*.c`, `pack-bitmap.c`, `midx.c`, `commit-graph.c`, `http.c`, …).
**Verdict:** Critical.
**Plan delta:** FR10 mandatory Stage 2 gates; inventory table in 004; never share files maintenance path.

### Pass 02 — git-core: receive-pack quarantine already exists

**Attack:** Inventing a parallel quarantine without integrating `odb_transaction` / receive-pack quarantine will double mechanisms and miss hooks.
**Evidence:** `git-receive-pack` quarantine docs; `odb_transaction_begin(..., ODB_TRANSACTION_RECEIVE)` in `builtin/receive-pack.c`; `odb_set_temporary_primary_source`.
**Verdict:** High — design must map cloud generations onto existing txn/quarantine concepts.
**Plan delta:** Stage 3 explicitly: implement cloud via `odb_source` txn callbacks + generation id, not a side door that bypasses receive-pack.

### Pass 03 — consistency: blob-then-meta race

**Attack:** Meta insert before blob visible ⇒ readers see missing objects.
**Evidence:** Mononoke write-once blob discipline; CAS systems generally put immutable bytes first.
**Verdict:** Critical invariant.
**Plan delta:** Already Decision 7; add test “read after meta without blob must be impossible via API helper.”

### Pass 04 — consistency: meta without ref publish leak

**Attack:** Successful object writes + failed ref CAS leave permanent orphans → unbounded cost.
**Evidence:** Mononoke ephemeral bubbles expire together; receive-pack migrates quarantine only on success.
**Verdict:** High.
**Plan delta:** Quarantine generation required; sweeper Stage 6 not optional for production; Stage 3 must TTL-drop open gens.

### Pass 05 — product: server reflog is a trap

**Attack:** Implementing full reflog “for git compatibility” blows MySQL write QPS and still fails forge audit needs (actor, push id, force).
**Evidence:** Bare repos often lack useful reflog; Mononoke `bookmarks_update_log` is the real pattern; user direction.
**Verdict:** Accept no required reflog.
**Plan delta:** Confirmed M1; `ref_updates` columns must include actor/push_id/reason (add to schema playbook).

### Pass 06 — cache: Redis as accidental SoT

**Attack:** Tip served from Redis after MySQL rollback/restore split-brain.
**Evidence:** Standard cache coherence failures; user constraint long-term SoT MySQL+blob.
**Verdict:** High.
**Plan delta:** Invalidate-after-commit only; version token in cache key; any Redis tip must be verified on authz-sensitive ops optional flag default off for writes.

### Pass 07 — perf: per-oid S3 clone death

**Attack:** 10M-object clone does 10M GETs → timeout/cost.
**Evidence:** GitHub pack internals series; JGit DFS pack-centric; Mononoke delta manifests/bundle URI; GitLab bundle URI support.
**Verdict:** Critical for production perf claims.
**Plan delta:** Stage 7 blocking before “prod perf”; Stage 3 only claims functional protocol.

### Pass 08 — perf: missing commit graph index

**Attack:** have/want negotiation parses every commit blob from S3.
**Evidence:** Mononoke SQL commit graph; git commit-graph on disk today.
**Verdict:** High.
**Plan delta:** FR13 `commit_parents` at write; seal walks use SQL first.

### Pass 09 — prior art: gitgres bytes-in-SQL

**Attack:** “Simplify” by storing zlib objects in MySQL BLOBs.
**Evidence:** gitgres demo path; DB backup/TOAST pain at scale.
**Verdict:** Reject.
**Plan delta:** M2-adjacent already M via C2; schema playbook forbids payload columns.

### Pass 10 — prior art: Gitea/GitLab disk gravity

**Attack:** Under schedule pressure, “just keep objects on local disk like Gitea and only put LFS in S3.”
**Evidence:** Gitea architecture; GitLab epic #479 still open conceptually for full ODB.
**Verdict:** Scope discipline.
**Plan delta:** Non-goal explicit; scratch disk cannot become SoT (check in sweeper: no durable path under gitdir/objects).

### Pass 11 — HA: single MySQL primary

**Attack:** MySQL outage freezes all ref updates / existence checks.
**Evidence:** Mesh/Gitaly scale by sharding nodes; Mononoke multiplex blob + SQL HA assumptions.
**Verdict:** Medium for v1, High for multi-tenant prod.
**Plan delta:** Document failure domain; v1 single-primary OK; Stage 8 runbook: read-only mode if meta down; blob multiplex deferred.

### Pass 12 — HA: S3 regional failure

**Attack:** Single-bucket loss.
**Evidence:** Mononoke multiplexedblob + healer + WAL.
**Verdict:** Medium deferred.
**Plan delta:** Phase-4+ multiplex optional; v1 accept single bucket + versioning; note healer pattern from Mononoke docs.

### Pass 13 — security: cross-tenant oid collission/fetch

**Attack:** Global key `objects/<oid>` allows cross-repo read if authz only at repo ref level.
**Evidence:** Content-addressed dedupe temptation; tenancy bugs common.
**Verdict:** Critical.
**Plan delta:** Decision: **per-repo blob key prefix** `git/v1/<repo_id>/objects/<oid>` in v1 (dedupe later explicitly). FR14 strengthened.

### Pass 14 — security: cache poisoning existence

**Attack:** Negative cache “oid missing” blocks read after write.
**Evidence:** Classic CDN/cache bug.
**Verdict:** Medium.
**Plan delta:** Short negative TTL; writes must bust existence key; prefer not caching negatives in v1.

### Pass 15 — ops: sweeper deletes live objects (false unreachable)

**Attack:** Seal race with concurrent push publishes new tip referencing gen still being swept.
**Evidence:** GC races in CAS systems; need generation publish barriers.
**Verdict:** Critical.
**Plan delta:** Rule: never sweep generation ≥ min(open gens); tips must only reference sealed+published objects OR open gen of same push txn; cross-push must wait until objects in published state.

### Pass 16 — ops: no walker/scrub

**Attack:** Silent missing blobs; fsck absent.
**Evidence:** Mononoke walker scrub/validate (`docs/6.2-walker-and-validation.md`).
**Verdict:** High for prod.
**Plan delta:** Stage 8+ `cloud scrub` job: tips→graph→blob exists; not Stage 1 blocker but required before multi-tenant.

### Pass 17 — compat: alternates and object sharing

**Attack:** Git alternates break when sources aren’t files paths.
**Evidence:** `odb_source` alternates API path-based.
**Verdict:** Medium.
**Plan delta:** Cloud v1 **no alternates**; `read_alternates` empty; document non-support.

### Pass 18 — compat: shallow/partial clone

**Attack:** Promisor/remote object fetch assumes HTTP object paths or pack capabilities mismatched.
**Evidence:** partial clone complexity in git; mononoke LFS separate.
**Verdict:** Medium.
**Plan delta:** Stage 3 acceptance: full clone/fetch/push only; partial clone explicit later matrix.

### Pass 19 — compat: SHA-256 and algo mixed repos

**Attack:** `BINARY(20)` oid columns break sha256.
**Evidence:** git extensions objectformat; oid 32 bytes max.
**Verdict:** High if wrong schema.
**Plan delta:** schema `oid BINARY(32)` + `hash_algo` on repo; already in draft schema notes.

### Pass 20 — cost: small-object PUT amplification

**Attack:** Millions of tiny S3 objects → API cost dominates storage.
**Evidence:** JGit DFS packs; Mononoke packblob; industry pack economics.
**Verdict:** High at scale.
**Plan delta:** Stage 7 pack envelopes also as **storage packing** not only clone accel; consider write-path mini-pack for small objects in Stage 7 design notes.

### Pass 21 — git-core: commit-graph writer assumes files

**Attack:** Something triggers commit-graph write → downcast.
**Evidence:** `commit-graph.c` downcast usages.
**Verdict:** High.
**Plan delta:** Disable automatic commit-graph write in cloud mode; SQL parents replace it for server.

### Pass 22 — forge-SRE: multi-ref push atomicity

**Attack:** Push updates main+tags; partial apply leaves inconsistent set.
**Evidence:** git ref transactions; Mononoke multi bookmark txn modules.
**Verdict:** High.
**Plan delta:** Single SQL transaction for all ref updates in one receive-pack txn; matches `ref_transaction` finish.

### Pass 23 — forge-SRE: force-push restore UX

**Attack:** User wants previous main after force push; we dropped objects.
**Evidence:** Product expectation; bookmarks_update_log enables restore if objects kept.
**Verdict:** Product medium.
**Plan delta:** Default grace window (e.g. 7–30d) before sweep orphans; optional pin on default-branch force; document.

### Pass 24 — perf: upload-pack CPU noisy neighbor

**Attack:** Pack generation starves co-hosted repos on same node.
**Evidence:** Gitaly pack-objects concurrency controls (industry practice).
**Verdict:** Medium.
**Plan delta:** Stage 8 runbook: process limits; future daemon with worker pool; not storage SoT issue.

### Pass 25 — security: signed refs / quarantine hooks

**Attack:** Hooks need object access during quarantine before publish.
**Evidence:** receive-pack runs hooks while quarantine active.
**Verdict:** High.
**Plan delta:** Cloud ODB must make quarantine objects readable to same repo during txn (generation visibility rule).

### Pass 26 — research: Bundle URI path

**Attack:** Building only custom envelopes ignores upstream client feature.
**Evidence:** git bundle-uri docs; GitLab Gitaly bundle URI; git-bundle-server.
**Verdict:** Medium opportunity.
**Plan delta:** Stage 7 prefer **Bundle URI-compatible artifacts** where possible instead of only proprietary envelopes.

### Pass 27 — research: Dolt/CAS GC mark-sweep

**Attack:** Reference counting every edge is too heavy; mark-sweep from tips is enough.
**Evidence:** CAS GC literature; Dolt CAS chunk GC; mark-compact concepts.
**Verdict:** Affirm mark-from-roots + grace, not refcounts v1.
**Plan delta:** No refcount column required; sweeper is mark/sweep generational.

### Pass 28 — architecture: in-process git vs Gitaly-style RPC

**Attack:** Embedding MySQL pools in every `git upload-pack` process connection-thrashes.
**Evidence:** Gitaly long-lived service owns git compute; Mononoke servers long-lived.
**Verdict:** High at scale.
**Plan delta:** Stage 4 note: short-term spawnable git with conn cache; Stage 8+ recommended **long-lived cloud git daemon** owning pools (still using same ODB/refs code).

### Pass 29 — architecture: reftable vs cloud refs

**Attack:** Implementing cloud as “reftable in S3” to reuse code.
**Evidence:** reftable assumes stack compaction/files; bad fit for multi-writer SQL CAS.
**Verdict:** Reject.
**Plan delta:** Keep `refs_be_vex` SQL-native; do not store reftable as SoT.

### Pass 30 — red-team: delete repo semantics

**Attack:** repo delete leaves S3 bytes forever or sweeper cannot run without tips.
**Evidence:** tenancy lifecycle.
**Verdict:** Medium.
**Plan delta:** `repositories.deleted_at` + bulk generation sweepable; lifecycle playbook Stage 8.

### Pass 31 — red-team: clock skew TTL bubbles

**Attack:** Expiry based on wall clock with skew resurrects or early-deletes.
**Evidence:** distributed TTL issues; mononoke expires_at.
**Verdict:** Medium.
**Plan delta:** Prefer min expiry by server DB time; grace constants in MySQL; don’t trust client clocks.

### Pass 32 — git-core: odb_source path field

**Attack:** code assumes `source->path` is filesystem for logging/temp.
**Evidence:** `odb_source` has `char *path`; inmemory uses dummy path.
**Verdict:** Low-Medium.
**Plan delta:** Use synthetic path `vex://<repo_id>` like inmemory’s `"source"`.

### Pass 33 — cost: audit table infinity

**Attack:** ref_updates grows without bound.
**Evidence:** high frequency bot branches.
**Verdict:** Medium.
**Plan delta:** Retention job; partition by time; optional sample noisy ref namespaces.

### Pass 34 — consistency: symref cycles / HEAD

**Attack:** HEAD→main missing row.
**Evidence:** git always has HEAD.
**Verdict:** Medium.
**Plan delta:** Bootstrap HEAD symref row at repo create; FR bootstrap checklist Stage 2.

### Pass 35 — final synthesis: stage gating

**Attack:** Marketing “cloud git done” after Stage 3.
**Evidence:** passes 07,12,16,28.
**Verdict:** Process.
**Plan delta:** Explicit readiness levels in STATUS/001:

| Level | Meaning | Min stage |
|-------|---------|-----------|
| L0 | Design | 0 |
| L1 | Dev fs correctness | 3 |
| L2 | MySQL+S3 functional | 4 |
| L3 | Safe multi-tenant beta | 6 + scrub |
| L4 | Perf-competitive clone | 7 |
| L5 | HA hardened | multiplex + daemon |

---



### Pass 36 — git-core: odb_transaction is the real integration API

**Attack:** Implementing only `write_object` and ignoring `begin_transaction` / `odb_transaction.env` breaks receive-pack hooks and child processes that must write into quarantine.
**Evidence:** `odb/transaction.h` — transactions, `ODB_TRANSACTION_RECEIVE`, `odb_transaction_env` for child_process.env; receive-pack uses these extensively.
**Verdict:** Critical for Stage 3.
**Plan delta:** Stage 2/3 acceptance must include working `begin_transaction` on cloud source; env vars documenting push_id/generation for hooks/children.

### Pass 37 — prior art: Gitaly object quarantine doc

**Attack:** Diverging from git's quarantine semantics confuses forge hook authors (pre-receive sees objects that must not be permanent yet).
**Evidence:** Gitaly `doc/object_quarantine.md` explains receive-pack quarantine lifecycle; mirrors upstream git.
**Verdict:** High compat.
**Plan delta:** Document cloud quarantine = same lifecycle as git quarantine: visible during hooks, published on success, discarded on failure.

### Pass 38 — perf: latency amplification is not only clone

**Attack:** Even `cat-file` / hook tree walks / CI shallow fetches thrash under high RTT to S3 for many small objects.
**Evidence:** Industry reports on high-latency git and many-small-files; LFS concurrent transfer issues analogous.
**Verdict:** High for UX before L4.
**Plan delta:** Require **local process cache** (LRU of inflated objects) and optional on-disk scratch object cache with TTL in Stage 3+; Valkey not enough (too far for blob bytes). Add `vex_blob_cache` scratch tier.

### Pass 39 — consistency: single-flight txn limitation

**Attack:** `Only a single transaction may be pending at a time` per ODB — concurrent pushes to same repo process model.
**Evidence:** `odb/transaction.h` comment single pending txn.
**Verdict:** Medium — process model dependent.
**Plan delta:** Document: one receive-pack process per push (normal); long-lived daemon must serialize per-repo txns or multi-txn extension later.

### Pass 40 — red-team residual top outage mode

**Attack:** What actually pages first in prod?
**Evidence:** synthesis of passes 07,11,28,38.
**Verdict:** Most likely early outage: **upload-pack latency/timeout under per-oid S3 reads** (not data loss), second: **MySQL connection storms from short-lived git processes**.
**Plan delta:** Dogfood gate = L3 minimum for private forge data safety; public/perf dogfood needs L4 + daemon pooling (L5-ish). Explicit in 001.

### Pass 41 — schema: object state published vs quarantine query paths

**Attack:** upload-pack serves quarantine objects from another push.
**Evidence:** multi-tenant concurrent pushes.
**Verdict:** High.
**Plan delta:** Reads for non-owner sessions only `state=published`; in-txn reads include own `generation`/`push_id`.

### Pass 42 — API: explicit publish boundary

**Attack:** `freshen_object` or accidental write path auto-publishes.
**Evidence:** git freshen for gc survival.
**Verdict:** Medium.
**Plan delta:** freshen is metadata-only touch; cannot change quarantine→published without ref txn publish API.

### Pass 43 — market: Bundle URI table stakes

**Attack:** Proprietary-only envelope won't work with stock git clients optimally.
**Evidence:** upstream bundle-uri; GitLab Gitaly bundle URI support; git-bundle-server.
**Verdict:** Medium-High for clone UX.
**Plan delta:** Stage 7 primary artifact = bundles advertised via bundle-uri; proprietary envelopes secondary.

### Pass 44 — market: positioning risk

**Attack:** Science project vs Gitaly-on-disk which works today.
**Evidence:** entire industry still on block storage for ODB.
**Verdict:** Strategic.
**Plan delta:** Position as **L2+ storage engine for new forges / migration**, not drop-in week-one GitLab replacement; roadmap honesty in STATUS.

### Pass 45 — security: blob key enumeration

**Attack:** Predictable keys + overly broad bucket IAM allows oid fetch if oid guessed/leaked.
**Evidence:** SHA-1 known; still need IAM least privilege per prefix optional.
**Verdict:** Medium.
**Plan delta:** Bucket policy per prefix ideal; app must not issue unauthenticated blob URLs; no public-read.

---

## Simulated multi-agent consensus (subagents unavailable)

Routing to external subagent models failed in-session; local multi-role simulation produced passes 01–45. Consensus:

1. **Native Git objects: GO** for forge git compatibility; Bonsai is a multi-year tax.
2. **Dogfood private forge: L3** (sweeper+scrub+mysql+s3), not L1.
3. **Public clone perf claims: L4** with bundle-uri.
4. **Must-fix API gaps:** txn begin/env, generation-scoped reads, local blob LRU/scratch cache.
5. **Top residual risks:** short-lived process connection pools; pack CPU isolation; SHA-1 content policy still on git; hook ecosystem assumptions; delete/rename repo lifecycle.


### Pass 46 — product: stock client + git/git ≤1.5× GitHub is the real bar

**Attack:** Shipping L3 “safe storage” without stock HTTPS clone of git/git at ≤1.5× GitHub fails the user’s verification criteria; custom tooling would also fail.
**Evidence:** User requirement; smart HTTP must speak upload-pack/receive-pack; GitHub clone performance assumes packed bitmaps/edge; git/git ~80k commits.
**Verdict:** Critical product gate.
**Plan delta:** Added PRD 005; S2–S4 metrics; L4 = acceptance; dual SLO vs GitHub **and** local disk HTTPS control; FR15–FR18; Stage 7 hard gates; forbid custom client success path.

### Pass 47 — perf: localhost vs GitHub RTT cheat

**Attack:** Pass 1.5× GitHub solely because loopback RTT is ~0 while GitHub is transoceanic.
**Evidence:** Fairness concern obvious for localhost HTTPS.
**Verdict:** High methodology risk.
**Plan delta:** Dual gate `T_us ≤ 1.5 T_disk` on same frontend; publish bytes transferred; document bench network path to GitHub.

### Pass 48 — perf: without bitmap/precomputed pack, SLO impossible

**Attack:** Stage 7 “streaming pack from S3 objects” as p50 path will miss 1.5× on 80k commits.
**Evidence:** GitHub pack internals; prior passes 07/20/38.
**Verdict:** Critical engineering constraint.
**Plan delta:** L4 p50 path **must** be precomputed pack/bundle + bitmap-class structure; live packgen is fallback only.

---

### Pass 49 — ops: tests must not require real AWS

**Attack:** Design implies S3/MySQL but developers cannot run tests without cloud accounts → bitrot and unsharable benchmarks.
**Evidence:** User requirement for local S3 + MySQL via Docker Compose; controllable/shareable/benchmarkable.
**Verdict:** Critical process gate.
**Plan delta:** PRD 006; Stage 4 lands compose first; MinIO path-style endpoint; CI compose smoke; bench profile for git/git; ports on 127.0.0.1; same production drivers.

### Pass 50 — perf: CI cannot fairly run GitHub gate

**Attack:** Flaky CI fails L4 because `T_gh` varies with runner network.
**Evidence:** Shared CI RTT variance.
**Verdict:** High methodology.
**Plan delta:** CI enforces functional + \(T_{us}\le 1.5 T_{disk}\) on Compose; full GitHub gate on designated local/nightly bench host with `BENCH_INCLUDE_GITHUB=1`.

---

## Wave 3 — Ten-agent adversarial panel (passes 51–70)

Full write-up: [007-ten-agent-adversarial-panel.md](007-ten-agent-adversarial-panel.md).
External subagents unavailable (model 404); ten roles simulated in-process with the same attack→delta contract.

### Pass 51 — A1 storage: packs must be derived caches

**Attack:** Precomputed packs in MinIO diverge from tips after push; clones serve stale graphs or missing objects.
**Verdict:** Critical.
**Plan delta:** Packs/bundles = derived caches with invalidation (`max_object_generation` / tip generation); upload-pack only advertises covering packs (W3-1).

### Pass 52 — A1 storage: SQL-per-oid exists on clone

**Attack:** Fallback or careless packgen issues millions of MySQL exists() calls.
**Verdict:** Critical for SLO.
**Plan delta:** p50 clone path forbids SQL-per-oid; bitmap/pack idx memory or mmap (W3-2).

### Pass 53 — A2 git-core: thin gitdir discovery

**Attack:** Cloud repo without classic `objects/` fails `is_git_directory` / http-backend project discovery.
**Verdict:** High.
**Plan delta:** Thin gitdir still provides required markers (HEAD, config, refs stub, maybe empty objects dir) (W3-10).

### Pass 54 — A2 git-core: maintenance honesty

**Attack:** Operators run `git gc` expecting S3 optimization.
**Verdict:** High.
**Plan delta:** Safe-fail tests for gc/repack/midx/commit-graph write; message → pack rebuild job (W3-11).

### Pass 55 — A3 perf: clone data path

**Attack:** Any design that inflates all objects to build pack at request time fails git/git.
**Verdict:** Critical (reaffirms 48).
**Plan delta:** Document mandatory p50 flow: tips → covering pack → stream MinIO/mmap bytes (007 A3).

### Pass 56 — A3 perf: bundle-uri fallback cliff

**Attack:** Advertise bundle-uri but p50 clients take slow fallback.
**Verdict:** High.
**Plan delta:** Measure p50 with default stock git config; fallback path budget tested separately.

### Pass 57 — A4 security: Valkey/compose exposure

**Attack:** Open Redis/MinIO ports or default passwords on non-loopback.
**Verdict:** High.
**Plan delta:** CI assert 127.0.0.1 binds; Valkey auth in cache profile (W3-8).

### Pass 58 — A4 security: hook vs publish ordering

**Attack:** Objects/tips published before pre-receive completes → evil push accepted mid-hook.
**Verdict:** Critical.
**Plan delta:** Hook visibility to quarantine; publish only after successful hooks (align receive-pack order).

### Pass 59 — A5 SRE: CGI connection stampede

**Attack:** Short-lived git-http-backend per request exhausts MySQL.
**Verdict:** Critical for dogfood.
**Plan delta:** Long-lived git-http required before multi-user L3/L4 dogfood (W3-13).

### Pass 60 — A5 SRE: sweeper deletes pack-referenced oids

**Attack:** Sweep unreachable-from-tips object still inside advertised pack.
**Verdict:** Critical.
**Plan delta:** Sweeper roots = tips ∪ pins ∪ objects in **active advertised packs** (W3-14).

### Pass 61 — A5 SRE: backup/restore missing

**Attack:** Restore MySQL without MinIO (or reverse) → unrecoverable.
**Verdict:** High.
**Plan delta:** Stage 8 runbooks: PITR+versioning, restore order, scrub after restore (W3-7).

### Pass 62 — A6 protocol: shallow is P0 for CI

**Attack:** Claiming all ops while `clone --depth` fails breaks GitHub-like CI.
**Verdict:** High.
**Plan delta:** Compat tiers; L4 requires P0+P1 including shallow (W3-3).

### Pass 63 — A6 protocol: partial clone explicit non-goal L4

**Attack:** Scope creep into filter clones delays pack work.
**Verdict:** Process.
**Plan delta:** P2 partial clone deferred; listed non-support at L4.

### Pass 64 — A7 bench: reproducibility

**Attack:** Unpinned images / no seed manifest → meaningless SLO.
**Verdict:** High.
**Plan delta:** Pin digests; seed-manifest.json; bench metadata; documented disk repack flags (W3-9).

### Pass 65 — A8 cost: per-push full repack

**Attack:** Rebuild entire git/git pack on every master push → CPU/cost death.
**Verdict:** Critical at scale.
**Plan delta:** Debounced / incremental pack rebuild policy (W3-5).

### Pass 66 — A9 product: fetch-after-push cliff

**Attack:** Clone SLO green; first fetch after push terrible → users reject.
**Verdict:** High.
**Plan delta:** Secondary SLO fetch-after-push ≤2× disk (W3-4).

### Pass 67 — A9 product: push auth in compose

**Attack:** F2–F8 undefined without auth → cannot dogfood.
**Verdict:** High.
**Plan delta:** Compose basic auth (or equivalent) for push tests (W3-8).

### Pass 68 — A9 product: classic bare export escape

**Attack:** No escape hatch from cloud SoT when pager duty hits.
**Verdict:** Medium-High.
**Plan delta:** export-to-classic-bare tool Stage 8 (W3-12).

### Pass 69 — A10 schedule: pack spike early

**Attack:** Leaving bitmap/pack until month 5 guarantees miss.
**Verdict:** High.
**Plan delta:** Stage 7 spike parallel from Stage 4; reuse git pack/bitmap on local pack cache (W3-6).

### Pass 70 — A10 panel consensus gate

**Attack:** Celebrate L2 mysql+minio as done.
**Verdict:** Process.
**Plan delta:** STATUS: largest open hole = derived-pack lifecycle + long-lived serve; L4 remains acceptance.

---

## Wave 4 — Grok-4.5 ten-agent panel (passes 71–90)

**Source:** [008-grok45-ten-agent-panel.md](008-grok45-ten-agent-panel.md)
All agents spawned with `model: grok-4.5`.

### Pass 71 — A1: pack advertise/stream race
**Attack:** Push/sweeper mutates while client streams pack → corrupt clone.
**Severity:** Critical.
**Delta:** Sealed immutable packs only; epoch advertise; W4-2.

### Pass 72 — A1: dual-write MySQL↔S3 partial failure
**Attack:** S3 put OK, MySQL abort (or reverse) → phantom/orphan.
**Severity:** Critical.
**Delta:** Staging keys + saga/compensate + chaos tests W4-1.

### Pass 73 — A1: sweeper misses in-flight clone
**Attack:** GC deletes objects while clone downloads pack.
**Severity:** High.
**Delta:** Active clone leases pin packs/objects W4-3.

### Pass 74 — A2: silent corruption worse than crash
**Attack:** Downcasts no-op; midx/bitmap wrong; dual-write split brain.
**Severity:** Critical.
**Delta:** Prefer hard fail; experimental posture; W4-4.

### Pass 75 — A2: fork vs in-tree
**Attack:** Shipping in distros as experimental causes data loss.
**Severity:** High process.
**Delta:** No upstreamable/default claim until downcasts eliminated; optional backend only.

### Pass 76 — A3: gameable GitHub p50
**Attack:** Best-of-N, warm MinIO vs cold GH, skip negotiation time.
**Severity:** High methodology.
**Delta:** W4-8 raw trials, CV, fixed methodology; disk gate primary in CI.

### Pass 77 — A3: p50 must be precomputed pack stream
**Attack:** Reaffirms live pack-from-loose fails both SLOs.
**Severity:** Critical.
**Delta:** Already policy; add zero-copy/range GET note + circuit-break fallback.

### Pass 78 — A4: zip-bomb / pathological pack DoS
**Attack:** receive-pack CPU/memory death.
**Severity:** High.
**Delta:** Size/depth/tree limits + unpack isolation W4-5.

### Pass 79 — A4: content-address verify
**Attack:** Client-supplied names/keys trusted.
**Severity:** High.
**Delta:** Always hash-verify W4-6.

### Pass 80 — A5: pack freshness lag pages
**Attack:** Clients hit incomplete packs → 5xx storm.
**Severity:** Critical ops.
**Delta:** SLI pack_invalidation_lag; never advertise incomplete; runbook pack backfill.

### Pass 81 — A5: compose-shaped prod thinking
**Attack:** Miss LB, IAM, partitions, long-conn lifecycle.
**Severity:** High.
**Delta:** Stage 8 multi-conn soak; document compose ≠ prod.

### Pass 82 — A6: interop git versions
**Attack:** Works on one git, fails 2.39–2.45 matrix.
**Severity:** High.
**Delta:** W4-7 interop matrix in 005.

### Pass 83 — A6: atomic push + push-options
**Attack:** Partial multi-ref apply; CI push-options ignored.
**Severity:** High.
**Delta:** P0/P1 include atomic + push-options tests.

### Pass 84 — A7: bench fraud / CV
**Attack:** Cherry-picked p50; CV high.
**Severity:** High.
**Delta:** W4-8 schema + CV>15% fail flag + N prefer 10.

### Pass 85 — A8: S3 request economics
**Attack:** GET storm if not pack-centric; 14d grace bloat.
**Severity:** High cost.
**Delta:** W4-9 cost SLIs; lifecycle tombstones; pack-only p50 path.

### Pass 86 — A8: Gitaly unit economics dissent
**Attack:** Pure object-store loses hot-path $ vs disk Gitaly.
**Severity:** Strategic.
**Delta:** Accept dissent; optional hot NVMe tier later; not v1 scope cut of S3 SoT.

### Pass 87 — A9: concurrent CI dogfood
**Attack:** Green clone SLO, brown concurrent pushes.
**Severity:** High product.
**Delta:** W4-10 50-conn hammer + 7-day soak.

### Pass 88 — A9: force-with-lease / proxy auth
**Attack:** Semantics/proxy breaks trust.
**Severity:** Med-High.
**Delta:** Explicit F-tests for force-with-lease; document proxy/CA.

### Pass 89 — A10: week 1–2 spikes
**Attack:** Building features before consistency/pack unknowns.
**Severity:** High schedule.
**Delta:** W4-11 mandatory spikes before Stage 2 expansion.

### Pass 90 — A10: L4 go/no-go checklist
**Attack:** Sliding definition of done.
**Severity:** Process.
**Delta:** W4-12 STATUS checklist: kill-9 durable L2; scrub scale; pack prototype; SLO harness; no P0s.

---

## Scoreboard

| Severity | Open | Accepted/deferred | Closed via plan delta |
|----------|------|-------------------|------------------------|
| Critical | 0 | 0 | …+93,94,97 |
| High | 0 | 11,12,86 | …+95,96,98–108 |
| Medium | 0 | 18,24,30,31,33,63 | prior mediums |

Criticals all have plan deltas landed in 001/004/005/006/007.

---

## Iteration → document updates checklist

- [x] Quarantine maps to receive-pack/odb_transaction (Pass 02)
- [x] Per-repo blob keys (Pass 13)
- [x] No alternates v1 (Pass 17)
- [x] commit_parents required (Pass 08)
- [x] Bundle URI preference in Stage 7 (Pass 26)
- [x] Readiness levels L0–L5 (Pass 35)
- [x] ref_updates actor/push_id (Pass 05)
- [x] Grace window for force-push (Pass 23)
- [x] Long-lived daemon note (Pass 28)
- [x] cloud scrub (Pass 16)
- [x] Disable files commit-graph write (Pass 21)
- [x] Derived pack invalidation + sweeper roots (51, 60)
- [x] p50 pack stream path / no SQL-per-oid (52, 55)
- [x] P0+P1 shallow + fetch-after-push SLO (62, 66)
- [x] Debounced pack rebuild (65)
- [x] Long-lived git-http before dogfood (59)
- [x] Compose auth + bind asserts (57, 67)
- [x] Backup/export escape (61, 68)
- [x] Early pack spike schedule (69)
- [x] Wave 4 grok-4.5 panel 71–90 / 008
- [x] Dual-write saga + chaos (72)
- [x] Sealed packs + clone leases (71, 73)
- [x] Experimental not upstream default (75)
- [x] Bench schema/CV + interop matrix (84, 82)
- [x] Dogfood hammer/soak + L4 go/no-go (87, 90)

---

## Evidence Log

### 2026-08-05

- Passes 01–92 earlier; wave 5 adds 93–108 (**108**).
- Wave 4: 008 grok-4.5 ten-agent; Wave 5: 010 eight-agent focus on push-stress + dual-write + PoC scope.

## Open Questions spawned by review

- [x] Exact grace default days → **14**
- [ ] Whether default-branch force auto-pins previous tip for grace
- [x] Daemon RPC shape → **long-lived smart HTTP required for L3/L4 dogfood**; multi-node HA later
- [ ] Incremental pack algorithm details (multipack vs single rolling pack)
- [ ] Exact debounce knobs for pack rebuild (time vs commit count)
- [x] Tenancy / auth product → **explicitly out of scope** (experiment/PoC only; compose shared push password OK)

### Pass 91 — scope: PoC not multi-tenant product

**Attack:** Security/SRE panel items pull the project into full forge tenancy/auth.
**Evidence:** User direction — experiment/PoC; tenancy and auth level stuff out of scope.
**Verdict:** Process freeze.
**Plan delta:** STATUS + 001/004/005/006 mark tenancy & auth product OOS; keep only engineering hygiene (repo_id keys, localhost binds, trivial push password for tests); demote multi-tenant threat model from PoC exit.

### Pass 92 — verify: clone-only harness misses push path

**Attack:** L4 clone SLO + mirror import never exercise thousands of small receive-pack txns.
**Evidence:** User request for empty→1-by-1/batch replay of git/git.
**Verdict:** High coverage gap.
**Plan delta:** PRD 009 incremental push stress harness; CI smoke batch=1; ramp/chaos at L3; maps to quarantine/ref CAS/pack invalidate stress.

---

## Wave 5 — post push-stress panel (passes 93–108)

**Source:** [010-wave5-adversarial-panel.md](010-wave5-adversarial-panel.md) (8× grok-4.5).

### Pass 93 — Storage: dual-write under batch=1 storm
**Delta:** Push **epoch fencing** + whole-epoch compensate (W5-1).

### Pass 94 — Storage: fault inject in harness
**Delta:** S3/MySQL/kill between stages; assert recon (W5-2, W5-3).

### Pass 95 — Storage: don’t require packs for L2/L3
**Delta:** Sequence sealed packs after push correctness (W5-4). *Reject* cutting L4 packs entirely.

### Pass 96 — git-core: high-freq receive-pack latency first
**Delta:** Thin-pack cold-base test; concurrent storm profile (W5-7, W5-8).

### Pass 97 — git-core: objects-then-refs invariant under load
**Delta:** Epoch boundary aligns with ref advance only after objects durable.

### Pass 98 — Perf: push-stress vs clone SLO tension
**Delta:** Serve-mode note + debounce; soft batch=1 budgets (W5-11, W5-12).

### Pass 99 — Perf: required push metrics
**Delta:** e2e push hist, fetch-after-push, rebuild lag, objects/s, conn reuse (W5-9).

### Pass 100 — QA: harness blind spots
**Delta:** fsck/tip every verify; empty zero refs; anti-cheat checkpoint (W5-5, W5-6).

### Pass 101 — QA: CI vs nightly matrix
**Delta:** CI smoke only; nightly ramp/random/chaos/resume.

### Pass 102 — Security: PoC checklist only
**Delta:** Affirm drop auth product; keep binds/creds/quarantine/DoS limits.

### Pass 103 — Product: PoC done definition
**Delta:** STATUS PoC done bullets (W5-13).

### Pass 104 — Product: kill if cannot approach 1.5×
**Delta:** Keep clone SLO as exit; no substitute vanity metrics.

### Pass 105 — Impl: push-stress earlier
**Delta:** Land 009 with Stage 3/4 ASAP (W5-14).

### Pass 106 — Impl: cut-if-late list
**Delta:** Pack polish / advanced sweeper / compose UX cuttable; e2e honesty not.

### Pass 107 — SRE: push-stress failure modes
**Delta:** Day-1 metrics; hung-at-step runbook; compose limits (W5-9, W5-10).

### Pass 108 — Panel consensus freeze
**Delta:** Epoch+fault+recon before L4 claim; packs sequenced not deleted; PoC scope holds.
