# Wave 5 Adversarial Panel (post push-stress + PoC scope)

**Status:** Complete  
**Date:** 2026-08-05  
**Models:** grok-4.5 via task tool  
**Agents:** Storage, git-core, Perf, QA Harness, Security, Product, Impl Lead, SRE (8 agents)  
**Inputs:** PRDs 001–009 (PoC scope, push-stress 009, L4 clone SLO, dual-write, derived packs)

Passes **93–108** recorded in [003](003-adversarial-review-log.md).

---

## Consensus

| Topic | Verdict |
|-------|---------|
| PoC / no tenancy-auth product | Affirmed; security noise cut |
| Push-stress harness (009) | **Necessary**; clone-only coverage is insufficient |
| Dual-write under batch=1 storm | Still the **#1 correctness risk** |
| Derived packs | **Keep for L4 clone**, but **sequence after** push-stress correctness on loose/quarantine path |
| “Cut all packs for PoC” (Storage A1) | **Partial accept:** no pack dependency for L2/L3 push-stress gates; packs required for L4 clone SLO |
| One system both 1by1 push + fat clone | Tension real; debounce + mode-switch (serve sealed pack for clone, loose/incremental for fresh pushes) is the plan |
| PoC “done” | Stock git + push-stress + clone SLO + no corruption — not a forge product |

---

## Agent digests

### Storage
- P0: dual-write orphans/ghosts; ref CAS vs S3 order under concurrency  
- MUST: **push epoch fencing**; harness **fault inject** dual-write; **post-batch recon**  
- Cut for early PoC: don’t block L2/L3 on sealed packs  
- No-Go L4 until epoch+fault+recon exist  

### git-core
- High-freq receive-pack: cloud RTT in existence checks; txn/refs order; downcast races; fd pressure; hook view  
- Push-stress exposes **latency/lock collapse first**  
- Must test: concurrent storm+kill; thin-pack bases only in cloud  
- Biggest mistake: cloud on hot path without quarantine/cache boundary  

### Perf
- Push-stress invalidates “static sealed history” clone assumptions  
- Metrics: push e2e, fetch-after-push, rebuild lag, objects/s, CPU/RSS/IO, conn reuse, cache hits  
- Soft laptop batch=1: p50≤120ms p99≤400ms (calibrate)  
- Cannot jointly max both axes without serve-mode distinction  

### QA Harness
- Blind spots: first-parent only; resume vs locks; chaos without fsck; reader races; no large-blob path  
- Missing asserts: fsck+tip match every verify; metrics must include exit codes; empty target zero refs  
- Cheat: fake checkpoint/metrics without pushes  
- CI=smoke fixed small N; nightly=ramp/random/chaos/resume  

### Security (PoC-scoped)
- Still matters: localhost binds, creds, TLS, repo_id integrity, quarantine, push DoS limits  
- Drop: SSO/RBAC/WAF/compliance product  
- Attacks: basic-auth on bad TLS; prefix escape; pack bomb  

### Product
- Done = stock clone/push, push-stress survival, ≤1.5× clone, restart-safe, simple to run  
- Kill metric: cannot approach 1.5× after tuning  
- Refuse: auth/tenancy/UI/LFS/protocol invention  

### Impl Lead
- Path: L1 → **push-stress early** → L2 → L3 → L4 packs+clone  
- Week after spikes: L1 + instrumented 009  
- Cut-if-late: pack polish, advanced sweeper, compose UX — keep e2e honesty  

### SRE
- Failures: conn thrash, MySQL locks, MinIO small-PUT amp, debounce stampede, sweeper lag disk full  
- Day-1 metrics/logs with push-id  
- Runbook: hung at step i  
- Compose CPU/mem/pid/nofile limits  

---

## MUST-ADD deltas (W5)

| ID | Delta | Target |
|----|-------|--------|
| W5-1 | **Push epoch fencing**: one generation/epoch per receive-pack success boundary; compensate whole epoch | 001 write path |
| W5-2 | Push-stress **fault injection**: S3 fail / MySQL rollback / kill between stages | 009 |
| W5-3 | **Post-batch reconciliation** assert (tips, object index, blob exists sample) | 009 |
| W5-4 | **Sequence:** L2/L3 push-stress gates on quarantine+loose path; sealed packs not required until L4 clone | 004 |
| W5-5 | Harness **anti-cheat**: plan_sha + per-step push pid/trace; reject resume without matching tip | 009 |
| W5-6 | **fsck/tip assert** on verify_every; empty repo starts zero refs | 009 |
| W5-7 | Concurrent push-storm test (multi driver) optional profile | 009 |
| W5-8 | Thin-pack cold-base test (bases only on server) | 005/009 |
| W5-9 | Day-1 metrics: push latency hist, mysql locks, minio PUT, pack queue, disk/inode | 006/001 |
| W5-10 | Compose **resource limits** for stress profiles | 006 |
| W5-11 | Soft batch=1 latency budgets (calibrate; not L4 hard gate) | 009 |
| W5-12 | Serve mode note: clone uses sealed pack; recent pushes may be incremental until debounce | 001 |
| W5-13 | PoC done definition (product) in STATUS | STATUS |
| W5-14 | Impl path: land 009 smoke as soon as receive-pack works on fs/compose | 004 |

---

## Explicit non-deltas (refuse scope creep)

- Multi-tenant authz product  
- Cutting L4 pack work entirely (clone SLO needs it)  
- Requiring production HA for PoC success  

---

## Evidence

Eight grok-4.5 adversarial task runs, 2026-08-05, after 009 push-stress design and PoC scope freeze.  
