# Ten-Agent Adversarial Panel (Wave 3)

**Status:** Complete (in-process panel; external subagent routing unavailable)  
**Date:** 2026-08-05  
**Method:** Ten distinct adversarial roles reviewed the frozen plan (PRDs 001–006). Each role produced attacks, mandatory deltas, and a go/no-go fragment. Deltas are merged into 001/003/004/005/006/STATUS via this wave (passes 51–70 in 003).

**Note:** Configured `task_*` subagents failed with model 404 / concurrency limits. Panel executed as structured multi-role review with the same deliverable contract.

---

## Panel roster

| # | Role | Primary question |
|---|------|------------------|
| A1 | Storage architect | Is MySQL+S3 the wrong split for git packs? |
| A2 | git-core maintainer | Will in-tree cloud backends destroy maintainability? |
| A3 | Performance engineer | Can L4 1.5× gates actually pass? |
| A4 | Security engineer | Tenancy, quarantine, compose, cache |
| A5 | Forge SRE | First-week production outages |
| A6 | Protocol/compat engineer | “All git operations” overclaim |
| A7 | Benchmark scientist | Methodology fraud / irreproducibility |
| A8 | Cost engineer | MinIO/S3 + MySQL unit economics on git/git |
| A9 | Product adversary | What ships that users still reject? |
| A10 | Implementation lead | 6-month small-team schedule realism |

---

## A1 — Storage architect

### Attacks
1. **Pack SoT ambiguity:** If L4 serves precomputed packs from MinIO, is the pack the SoT or objects? Divergent updates after push break clones until rebuild.  
2. **MySQL object index cardinality:** git/git ~millions of objects → hot `objects` table; every exists() during fallback packgen kills p50.  
3. **commit_parents incomplete:** Grafts, replace refs, shallow boundaries not modeled.  
4. **Generation sweep vs pack membership:** Sweeper deletes oid still inside advertised bundle.  
5. **MinIO ≠ S3:** List/consistency/IAM differences hide prod bugs.

### Mandatory deltas
- **D1. Pack envelope consistency model:** packs are *derived caches* with `pack_generation` / `max_object_generation` / `invalidated_at`; upload-pack may only advertise packs covering all published tips.  
- **D2. Exists path must not SQL-per-oid on clone p50:** bitmap/pack idx in memory or local mmap scratch; SQL exists only for miss/repair.  
- **D3. Dual-target blob CI:** MinIO daily + optional Localstack/AWS path later; document S3 API subset used.

### Gets right
Split mutable tips (SQL) from immutable bytes (blob) is correct.

### Go/No-Go fragment
**Go** if packs are derived and invalidated on tip move; **No-Go** if packs are hand-waved as “also in S3” without invalidation.

---

## A2 — git-core maintainer

### Landmines
1. **`odb_source_files_downcast`** in gc/repack/midx/bitmap/commit-graph — hard BUG or wrong path.  
2. **`packfile.c` / bitmap writers** assume local `.keep`/`.bitmap` beside packs.  
3. **`http-backend` + `GIT_PROJECT_ROOT`** multi-repo discovery assumes directories on disk.  
4. **`odb_transaction.env`** child hooks (`pre-receive`) expect quarantine env vars/filesystem layout.  
5. **promisor/partial clone** + cloud source interaction undefined → silent incomplete fetches.  
6. **reftable migration tools** / `git refs migrate` don’t know `cloud`.  
7. **index-pack --stdin** receive path must write via cloud txn not loose tmp under `.git/objects`.  
8. **`setup.c` bare repo heuristics** (`objects/`, `HEAD` file) — cloud thin gitdir may fail `is_git_directory`.

### Silent vs crash
| Silent wrong | Hard crash |
|--------------|------------|
| gc no-op leaving operators thinking repack helped | downcast BUG |
| commit-graph absent → slow negotiate not error | midx write die |
| hooks missing quarantine objects | open() on synthetic path |

### Extra tests git-core would demand
- `pre-receive` hook `git cat-file` quarantine oid  
- concurrent push to two branches atomicity  
- `git repack` / `gc` / `commit-graph write` / `multi-pack-index` under cloud (must safe-fail)  
- `git clone --depth` + deepen  
- `git fetch` after force-push (have/want)  
- fsck connectivity  
- alternate object dir rejected cleanly  

### Verdict
**Optional in-tree backend** behind extensions OK; treat as experimental. **Do not** claim “full git maintenance compatibility.” Prefer thin gitdir + explicit `git cloud-*` operator tools over lying that `git gc` optimizes S3.

---

## A3 — Performance engineer

### Gameable GitHub gate
- Localhost RTT vs GitHub WAN (already partially mitigated by disk gate).  
- Comparing to throttled/unauthenticated GitHub vs paid edge.  
- Warm OS page cache for pack file on server after “cold client” only.  
- Serving fewer refs/tags than GitHub default clone.  
- Huge CPU turbo on bench laptop not documented.

### Disk gate still fails if
- Pack deltas worse than `git repack -adf` on disk control.  
- Extra MySQL RTT before first byte.  
- Re-compressing pack on each clone instead of `sendfile`/blob stream.  
- TLS termination + CGI spawn per request.  
- Bundle-URI advertised but client version ignores; fallback path slow and becomes p50.

### Required clone data path (p50)
```text
client GET info/refs
  → git-http (long-lived)
  → ref tips from memory/Valkey/MySQL (batched)
client fetch wants
  → select covering pack/bundle (MySQL pack index)
  → stream pack bytes from MinIO (ranged GET) OR local mmap cache of pack
  → sideband progress
  → NO per-oid SQL; NO inflate-all-objects
```

### Minimum derived data for git/git
1. One or few **high-quality packs** (or bundle) covering default clone set  
2. **Pack index** + **reachability bitmap** (or multi-pack bitmaps)  
3. Tip → pack covering set map  
4. Optional bundle-uri JSON  
5. HEAD/ref advertisement snapshot for info/refs cache  

### Prediction
**Streaming pack-from-objects only: FAIL** both gates.  
**Precomputed pack stream + long-lived server: PASS disk gate probable; GitHub gate probable on good NIC.**

---

## A4 — Security engineer

### Top attacks
1. Cross-repo read via shared MinIO principal + guessed oid.  
2. Quarantine oid readable by concurrent fetch (generation leak).  
3. Valkey tip poisoning if unauthenticated Redis port published.  
4. Compose default passwords on non-loopback bind.  
5. `pre-receive` race: publish before hook finishes (TOCTOU).  
6. Hook escapes to read other repos’ scratch packs.  
7. Force-push hides malware; audit without pin → evidence GC’d.  
8. Bundle URI open redirect / SSRF if URI attacker-controlled.  

### Checklist mitigations (roadmap)
- [ ] MinIO/S3 credentials never on client  
- [ ] Compose ports `127.0.0.1` only (already) + CI assertion  
- [ ] Valkey password + localhost  
- [ ] Generation-scoped object visibility tests  
- [ ] Hook runs before publish; failure aborts generation  
- [ ] Bundle URIs only server-controlled HTTPS endpoints  
- [ ] Scratch dir per-push with 0700 and scrub on complete  
- [ ] ref_updates immutable insert (no update/delete from app)  

### Never on client
AWS keys, MySQL DSN, Valkey, repo UUID admin APIs, sweeper controls.

---

## A5 — Forge SRE

### First-week outages
1. MySQL max_connections from CGI git-http spawn storm.  
2. Disk full on scratch volume during concurrent clones.  
3. MinIO slow → upload-pack timeouts → client retries stampede.  
4. Sweeper bug deletes objects still in advertised pack.  
5. Sealer lag → push OK but fetch missing parents (if publish broken).  
6. Certificate expiry on localhost-style prod misconfig.  

### Page SLIs
- `receive_pack_success_rate`  
- `upload_pack_ttfb_p95`  
- `upload_pack_error_rate`  
- `mysql_connections` / saturation  
- `minio_5xx`  
- `pack_invalidation_lag` (tips newer than any covering pack)  
- `scratch_volume_used_ratio`  

### Backup gaps
- MySQL PITR + MinIO versioning **both** required; restore drill missing.  
- Order: restore meta then blobs; or accept generation holes with scrub.  
- ref_updates backup for forensics.  

### Missing jobs/runbooks
- pack rebuild on tip move  
- scrub+repair  
- connection pool dashboards  
- “disable sweeper” breakglass  
- export-to-classic-bare disaster escape  

### Compose mental model risk
**Yes** — single-node compose hides connection pooling and multi-instance pack cache coherence. Prod needs daemon + shared pack cache rules before multi-node.

---

## A6 — Protocol / compat engineer

### Overclaim: “all git operations”
Local commit/rebase never hit server — OK. Remote gaps:

| Feature | Risk if ignored |
|---------|-----------------|
| protocol v2 ls-refs prefixes | slow/wrong advertisement |
| shallow / deepen / unshallow | CI breakage |
| partial clone filters | modern GH UX gap (defer OK if explicit) |
| atomic multi-ref push status | partial apply horror |
| tag follow / annotated tags | clone -b issues |
| ref namespaces / HEAD unborn | empty repo bootstrap |
| push options | forge hooks |
| HTTP auth + 401/403 credential helper | stock client UX |
| range-diff / bundle create from client | rare |

### Mandatory deltas
- **Compat matrix tiers:** P0 (clone/fetch/push/ff/force-with-lease/tags/atomic), P1 (shallow), P2 (partial), P3 (oddities).  
- L4 requires **P0+P1** on git/git, not only full clone.  
- Explicit **non-support list** shipped with release notes.

---

## A7 — Benchmark scientist

### Irreproducibility vectors
- Unpinned MinIO/MySQL images  
- No isolation of CPU frequency / thermal  
- GitHub rate limit mid-bench  
- Mirror not refreshed; HEAD mismatch silent  
- N=5 too small without variance report  
- Disk control pack not `git repack -adf --window=250` quality  

### Mandatory deltas
- Pin compose image digests  
- `seed-manifest.json` with HEAD, object count, pack sha  
- Bench records: git version, cpus, mem, docker version, governor if available  
- Disk control built with documented `repack` flags  
- Statistical rule: pass if p50 gate met **and** p95_us ≤ 2.0× p50_gh (tail control) optional soft  
- Warmup clone discarded  

---

## A8 — Cost engineer

### Cost bombs
1. Millions of tiny MinIO objects PUT on push without packing.  
2. MySQL `ref_updates` infinite growth.  
3. Rebuilding full git/git pack on every push to master.  
4. Versioning + no sweeper → storage monotone up.  
5. Cross-AZ MySQL chatty exists().  

### Mandatory deltas
- Pack rebuild policy: debounce (e.g. every N minutes or every M commits), not per-push full rebuild.  
- Incremental pack + bitmap update strategy note in Stage 7.  
- Object storage classes / lifecycle for swept tombstones.  
- Budget dashboard: objects count, blob bytes, pack bytes, mysql size.

---

## A9 — Product adversary

### User rejection modes
1. “Works on compose” but push auth story undefined → cannot dogfood.  
2. Clone fast, **fetch after first push** slow (pack invalidation gap).  
3. `git gc` does nothing; monorepo ops team panics.  
4. No migration from existing bare repo.  
5. Error messages expose MySQL/MinIO internals.  

### Mandatory deltas
- **Push auth** in compose (basic auth or ssh later) documented for F2–F8.  
- **Fetch-after-push** benchmark secondary SLO: p50 fetch ≤ 2× disk.  
- **Importer:** `git cloud import` / push --mirror path as first-class Stage 4/7.  
- Operator UX: gc message points to `cloud optimize` / pack rebuild.  
- Sanitize errors to clients (no DSN).

---

## A10 — Implementation lead (6 months, small team)

### Reality check
| Track | Months | Notes |
|-------|--------|-------|
| fs store + refs/odb wiring | 1–1.5 | L1 |
| compose mysql+minio + push/clone | 1 | L2 |
| quarantine correctness + sweeper/scrub | 1 | L3 |
| pack/bitmap/bundle + daemon + git/git SLO | 2–2.5 | L4 |
| polish/auth/docs | 0.5 | |

**Critical path = L4 pack machinery**, not MySQL schema.

### Schedule risks
- Underestimating receive-pack txn + hook env  
- Bitmap format work vs reusing on-disk bitmap beside cached pack file  
- One engineer blocked on protocol + storage  

### Verdict
**Conditional Go** for L4 in ~6 months with 2–3 strong engineers if:
- reuse git’s pack/bitmap code on **locally cached pack files** (derived from MinIO) rather than reimplementing bitmaps in SQL;
- freeze P2 partial clone out of scope;
- start Stage 7 design spikes during Stage 4.

**No-Go** if team is 1 junior or L4 pack work starts only after month 5.

---

## Consolidated mandatory plan deltas (wave 3)

| ID | Delta | Target PRD/stage |
|----|-------|------------------|
| W3-1 | Packs/bundles are **derived caches** with invalidation tied to tip generation | 001, 004 S7 |
| W3-2 | Clone p50 path: **stream precomputed pack**; no SQL-per-oid | 001, 005 |
| W3-3 | Compat tiers P0–P3; L4 = P0+P1 | 005 |
| W3-4 | Fetch-after-push secondary SLO | 005 |
| W3-5 | Pack rebuild debounce policy | 004 S7 |
| W3-6 | Prefer **mmap local pack cache** + stock bitmap code | 001 architecture |
| W3-7 | Backup/restore + pack rebuild + breakglass runbooks | 004 S8 |
| W3-8 | Compose auth for push tests; sanitize client errors | 006, 005 |
| W3-9 | Pin image digests; seed-manifest; bench metadata | 006, 005 |
| W3-10 | Thin gitdir must still pass `is_git_directory` / http-backend discovery | 001 S2 |
| W3-11 | Explicit experimental; gc/repack safe-fail tests | 004 S2 |
| W3-12 | Export-to-classic-bare escape hatch | 004 S8 |
| W3-13 | Connection pool / long-lived git-http required before multi-user dogfood | 001 L3/L4 |
| W3-14 | Sweeper must not delete objects referenced by **active advertised packs** | 001 S6 |

---

## Panel score summary

| Role | L4 feasible? | Harshest blocker |
|------|--------------|------------------|
| A1 Storage | Yes w/ pack invalidation | Pack vs object split confusion |
| A2 git-core | Yes experimental | downcast + thin gitdir discovery |
| A3 perf | Yes w/ precomputed pack stream | Live packgen |
| A4 security | Yes w/ checklist | quarantine leak / redis bind |
| A5 SRE | Yes after runbooks | conn stampede + sweeper/pack race |
| A6 protocol | Yes if P2 deferred | shallow ignored |
| A7 bench | Yes w/ pins/manifests | unreproducible numbers |
| A8 cost | Yes w/ debounce | per-push full repack |
| A9 product | Yes w/ auth+fetch SLO | fetch after push cliff |
| A10 impl lead | Conditional 6 mo | late pack work |

**Panel consensus:** Continue. Plan is directionally right. **Largest hole entering wave 3:** derived-pack lifecycle (build, advertise, invalidate, sweep interaction) and **long-lived serve path**. Address before writing substantial non-pack code beyond L2.

---

## Evidence

- In-process 10-role panel 2026-08-05 after subagent tool failure.  
- Inputs: PRDs 001–006, prior passes 01–50, git odb/refs/transaction knowledge, mononoke/jj prior art.  
