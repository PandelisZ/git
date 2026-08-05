# Ten-Agent Adversarial Panel — Grok-4.5 (Wave 4)

**Status:** Complete  
**Date:** 2026-08-05  
**Models:** All agents run with `model: grok-4.5` via task tool  
**Roles:** A1 Storage, A2 git-core, A3 Perf, A4 Security, A5 SRE, A6 Protocol, A7 Benchmark, A8 Cost, A9 Product, A10 Impl Lead  

This supersedes the in-process simulation in [007](007-ten-agent-adversarial-panel.md) as the **authoritative multi-agent review**. 007 remains historical. New passes **71–90** land in [003](003-adversarial-review-log.md).

---

## Executive consensus

| Axis | Consensus |
|------|-----------|
| Architecture direction (SQL tips + blob bytes + derived packs) | **Right** |
| L4 = stock git + git/git ≤1.5× | **Keep**, but methodology easy to game |
| 6-month small-team L4 | **Split:** Impl lead conditional Go; Storage/Cost harsher No-Go |
| Biggest engineering hole | **Cross-store consistency + pack lifecycle races + long-lived serve** |
| Biggest process hole | **In-tree vs fork**; maintenance downcasts |
| Ship strategy tension | Product: boring reliability first ↔ Perf: packs mandatory for SLO |

**Program decision after panel:** Continue roadmap; treat L4 as acceptance; add wave-4 deltas below; **do not** claim 6-month certainty; start pack+consistency spikes immediately.

**Scope update (user):** This remains an **experiment/PoC**. **Tenancy and auth products are out of scope.** Panel items about SSO/ACLs/multi-tenant IAM are backlog-at-best, not PoC exit criteria. Trivial shared Compose basic-auth for `git push` tests is enough.

---

## A1 — Storage Architect (grok-4.5)

### Top risks
1. **Critical** — Pack invalidation/advertising races during stream → corrupt/incomplete clone  
2. **Critical** — Dual-write MySQL↔S3 partial failure → phantom index or orphan bytes  
3. **High** — MySQL object-index scale under concurrent clone/push  
4. **High** — Sweeper root incompleteness (in-flight fetches, notes/replace, downloading packs)  
5. **High** — Quarantine vs stock multi-ref atomicity / concurrent fetch edges  

### MUST-ADD deltas
- **W4-1** Cross-store atomicity: staging keys + saga/compensate; chaos tests (kill mid-push, partition)  
- **W4-2** Pack lifecycle: sealed immutable packs only; MIDX + geometric/incremental; never advertise mutable packs; p99 fallback circuit-breaker  
- **W4-3** GC: lease/pin advertised packs **and active clone leases**; delayed tombstones; 10M-object concurrent benchmarks  

### Gets right
Native objects + smart HTTP + derived pack/bitmap caches.

### L4 6mo
**No-Go** for shippable forge storage; demo clone maybe.

---

## A2 — git-core Maintainer (grok-4.5)

### Landmines
1. odb files downcasts (~59)  
2. pack-bitmap / midx gen & verify  
3. receive-pack + odb_transaction + hooks  
4. ref_storage_be atomicity across mysql+s3  
5. http-backend + thin gitdir existence races  

### Silent vs crash
**Silent-wrong is majority and catastrophic** (corrupt reachability, dangling tips). Crashes are the good outcome.

### Extra tests
gc/repack/midx/bitmap verify; negotiation-tip; hook quarantine rollback mid-pack; concurrent push+prune; rebase/notes/replace; smart HTTP missing-object injection; crash mid dual-write then clone.

### Verdict
**Fork** until downcasts deleted and dual-write proven. In-tree experimental = silent data loss in distros.

**Program response:** Stay **out-of-tree / optional experimental** with explicit non-support of maintenance paths; track downcast elimination as hard gate before any “upstreamable” claim (W4-4).

---

## A3 — Performance Engineer (grok-4.5)

### GitHub gate gameable
Congested egress to GH vs warm local MinIO; skip info/refs timing; best-of-N; mirror lag; cold-start exclude abuse.

### Disk gate fails if
MySQL RTT, on-fly pack, no bitmap, debounced miss → slow fallback, CGI spawn, no zero-copy.

### Required p50 flow
long-lived git-http → cached refs → bitmap+MIDX negotiate → **precomputed pack only** → MinIO→socket stream (range/zero-copy).

### Min derived data
Closed pack(s)+idx, bitmap, MIDX if multi, refs snapshot, optional bundle + commit-graph.

### Streaming loose only
**Fail.**

---

## A4 — Security (grok-4.5)

### Top attacks
1. Zip-bomb / pathological pack DoS on unpack  
2. Cross-repo MinIO traversal  
3. Valkey/MySQL tip poisoning / SQLi  
4. Quarantine race promote before scan  
5. ref_update TOCTOU concurrent push  
6. Stolen compose creds  
7. Derived pack substitution  
8. Missing per-repo ACL / credential stuffing  

### MUST-ADD
- **W4-5** Inbound pack limits (size/depth/tree width) + unpack isolation/seccomp  
- **W4-6** Content-address verify always; never trust client names for storage keys  
- Fuzz git protocol + chaos races on ref_updates  

### Never to client
MinIO/MySQL/Valkey secrets, quarantine metadata, audit trails, storage layout extensions.

---

## A5 — Forge SRE (grok-4.5)

### First-week outages
1. Long-lived git-http FD/memory leaks; mid-fetch kills  
2. Pack gen lag → 5xx storms  
3. MySQL ref deadlocks  
4. MinIO throttle / consistency gaps  
5. Sweeper deletes live objects  
6. Compose-only tests miss LB/IAM/partitions  

### Page SLIs
success rate, p99 latency, pack freshness lag, mysql pool, S3 5xx/miss, sweeper deletes, active conn count  

### Gaps
Atomic multi-store snapshot; pack rebuild under thundering herd; conn drain runbook  

### Compose thinking → outages?
**Yes.**

---

## A6 — Protocol Compat (grok-4.5)

### Overclaim
“All ops” false; v0 fallback, sideband races, credential helpers, etc.

### Demanded split
- **P0:** smart HTTP up/receive v1/v2, ls-refs, thin-pack, auth, sideband errors  
- **P1:** shallow/deepen, push-options, atomic push, v2 caps (**filter deferred**)  
- **P2:** partial clone, bundle-uri, midx client, dumb HTTP  

### Wire traps
pkt-line partial reads; capability order; multi-ack modes; side-band demux; ls-refs peel/symref  

### MUST-ADD tests
v2 fetch+shallow+thin integrity; atomic push+delete+options; max-input/invalid pkt; concurrent ls-refs; **interop git 2.39–2.45** pack SHA+tips  

**W4-7** Interop matrix stock git versions; push-options + atomic in P0/P1.

---

## A7 — Benchmark Scientist (grok-4.5)

### Fraud vectors
Unseeded manifest; N&lt;5 cherry-pick; hidden cache/shallow; env drift; metric redefinition  

### JSON must include
benchmark_id, timestamp_utc, git_commit, manifest_seed, compose_hash, n_trials, raw trials[], p50s, baselines, ratios, env, optional_github_gate  

### Stats beyond p50
p90/p99, stdev/CV; CV&gt;15% unstable; prefer N≥10  

### W4-8
Harden bench schema + CV fail; warmup discarded; raw trials published.

---

## A8 — Cost Engineer (grok-4.5)

### Cost bombs
S3 request tax; 14d grace bloat; per-repo fixed tax; ref_updates write amp; dual pack+object storage  

### 10k repos red flags
Inactive repo fixed cost; request$ ≫ GB$; peak 3× storage on GC lag  

### Controls
- Throttle + pack-only serve + edge cache packs  
- Consider shorter grace or lifecycle to IA  
- Tiering: hot local NVMe optional later  

### Economics vs Gitaly
**No-Go** pure S3 for hot active git unit economics; S3 wins durability/ops at scale if request path is pack-centric.

**Program response:** Stay pack-centric (already); add **W4-9** cost SLIs (GET count/clone, blob bytes, mysql size); keep 14d grace but lifecycle tombstones; no global dedupe v1 still.

---

## A9 — Product Adversary (grok-4.5)

### Reject green L4
Monorepo OOM; concurrent CI ref races; HTTPS/proxy auth; force-with-lease semantics; no partial/LFS  

### Dogfood checklist
Daily clone/push/rebase on git/git + larger repo; 50-user hammer; ≤1.5× measure; identical refs; 7-day soak CI  

### Cut to ship trust
Cut fancy multi-region/UI; prove dumb smart-HTTP correctness — **but** packs still required for SLO (resolve: boring *protocol* + mandatory *derived pack cache*).

### Verdict
If stock git cannot treat it as dumb, fast, unbreakable remote on day one, green L4 is theater.

**W4-10** Dogfood: concurrent hammer + 7-day soak before L4 claim.

---

## A10 — Implementation Lead (grok-4.5)

### 6-month sketch (2–3 eng)
M1 L1 fs → M2–3 L2 mysql+minio → M4 L3 sweeper → M5–6 L4 packs+SLO  

### Killers
Pack iteration; distributed consistency; scrub cliffs; SLO mismatch; flaky CI  

### Week 1–2 spike
FS→MinIO→MySQL roundtrip+crash; naive pack+bitmap 10k objs; baseline git clone measure; failure harness  

### Go L4 only if
L1/L2 kill-9 safe; scrub 1M objs; pack prototype &gt;50% size reduction; eng free for SLO; no P0s  

**W4-11** Week 1–2 spikes mandatory before Stage 2 expansion.  
**W4-12** L4 go/no-go checklist formal in STATUS.

---

## Merged wave-4 roadmap deltas

| ID | Delta | Where |
|----|-------|--------|
| W4-1 | Dual-write saga/staging keys + chaos mid-push tests | 001, 004 S3–4 |
| W4-2 | Sealed immutable packs only; MIDX; geometric/incremental; circuit-break slow fallback | 001, 004 S7 |
| W4-3 | Active **clone leases** pin packs/objects during download | 001 S6–7 |
| W4-4 | No upstreamable claim until downcasts gone; experimental/out-of-tree posture explicit | 001, STATUS |
| W4-5 | Pack size/depth limits + unpack isolation | 004 S3, 006 |
| W4-6 | Always verify content hash on write/read path | 001 |
| W4-7 | Interop git 2.39–2.45; atomic push + push-options in matrix | 005 |
| W4-8 | Bench JSON schema + CV/p99 + raw trials | 005, 006 |
| W4-9 | Cost SLIs: S3 GET/clone, storage bytes, mysql size | 004 S8 |
| W4-10 | 50-conn hammer + 7-day soak dogfood gate | 005 |
| W4-11 | Week 1–2 consistency+pack spikes | 004 S0/S1 |
| W4-12 | Written L4 go/no-go checklist | STATUS |

---

## Dissent log (do not paper over)

| Agent | Dissent | Handling |
|-------|---------|----------|
| A1 Storage | No-Go L4 in 6 months | Accept uncertainty; conditional go via W4-12 |
| A2 git-core | Fork only | Experimental optional backend; no distro default |
| A8 Cost | S3 loses to Gitaly economics | Pack-centric + cost SLIs; hot-tier later optional |
| A9 Product | Cut fancy packing | Reject cut of packs (SLO requires them); cut multi-region/UI instead |

---

## Evidence

- Ten `task` invocations with `model: grok-4.5` (2026-08-05).  
- A9/A10 retried after scout concurrency limit.  
- Full verbatim themes captured above; raw tool transcripts in session.  
