# Roadmap Status

## Active PRDs

| ID | Title | Status | Stage | Updated | Notes | Next |
|----|-------|--------|-------|---------|-------|------|
| 001 | [Cloud-Scale Git Forge Storage](001-cloud-scale-git-forge-storage.md) | Draft | 0 - Discovery (freeze ready) | 2026-08-05 | **PoC/experiment**: MySQL+S3 storage; no tenancy/auth product. Stage 0 frozen. | Start Stage 1 when approved |
| 002 | [Comparative Research Digest](002-comparative-research-digest.md) | Draft | 0 - Research | 2026-08-05 | Prior art → constraints C1–C10 / M1–M4 | Add citations as found |
| 003 | [Adversarial Review Log](003-adversarial-review-log.md) | Draft | 0 - Review | 2026-08-05 | **90** passes; wave 4 grok-4.5 agents 71–90 | New passes only on design changes |
| 004 | [Stage Playbooks](004-stage-playbooks.md) | Draft | 0 - Planning | 2026-08-05 | L0–L5 gates; schema; per-stage tasks | Execute Stage 1 playbook next |
| 005 | [Stock Client Verification & Benchmark](005-stock-client-verification-and-benchmark.md) | Draft | 0 - Verification design | 2026-08-05 | Stock git only; git/git clone ≤1.5× GitHub and ≤1.5× disk control | Wire harness on Compose in Stage 7 |
| 006 | [Local Docker Compose Test Environment](006-local-docker-compose-test-environment.md) | Draft | 0 - Planning | 2026-08-05 | MySQL+MinIO(+Valkey)+git-http local stack; shareable scripts | Implement compose at Stage 4; bench profile Stage 7 |
| 007 | [Ten-Agent Adversarial Panel](007-ten-agent-adversarial-panel.md) | Superseded | Wave 3 | 2026-08-05 | In-process simulation (pre-grok routing) | Historical; see 008 |
| 008 | [Grok-4.5 Ten-Agent Panel](008-grok45-ten-agent-panel.md) | Complete | Wave 4 | 2026-08-05 | **Authoritative** 10 agents on grok-4.5; W4-1…W4-12; passes 71–90 | Execute week 1–2 spikes; L4 go/no-go checklist |
| 009 | [Incremental Push Stress Harness](009-incremental-push-stress-harness.md) | Draft | 0 - Harness design | 2026-08-05 | Empty→replay git/git via stock pushes; batch=1..N; metrics/resume/chaos/fault inject | Implement ASAP at Stage 3/4 |
| 010 | [Wave 5 Adversarial Panel](010-wave5-adversarial-panel.md) | Complete | Wave 5 | 2026-08-05 | 8× grok-4.5 agents; W5-1…W5-14; push-stress + dual-write focus | Epoch fencing; sequence packs after push correctness |

## Latest Evidence Notes

### 2026-08-05

- Git insertion points: `odb/source.h` callbacks, `refs/refs-internal.h` `ref_storage_be`, `extensions.refstorage` URI payloads in `setup.c`, `odb/source-inmemory.c` as non-files template.
- ~59 `odb_source_files_downcast` call sites (gc/repack/midx/bitmap/commit-graph/http) — cloud mode cannot share those paths blindly.
- Mononoke: blobstore + SQL metadata; ephemeral bubbles; bookmark update log; Git protocol/derived data.
- Jujutsu: `Backend` trait; thin WC; content-addressed store.
- Web/prior art: GitLab still block-storage bare repos (epic #479); Gitea git-on-disk + S3 only for LFS/artifacts; gitgres SQL objects/refs (avoid bytes-in-SQL); JGit DFS pack-centric; Bitbucket Mesh replicated disks; GitHub pack/delta/MIDX economics; Piper centralized DB (Bigtable/Spanner); BucketGit/s3-git-server naïve S3 patterns.
- Product frame: **forge/hosting**. No required server reflog; `ref_updates` audit. Valkey non-SoT. Disk scratch OK. Long-term SoT = **MySQL + blob only**.
- Adversarial review: **45 passes** in `003`. Extra research: in-tree `odb_transaction` quarantine; Gitaly object-quarantine doc; bundle-uri ecosystem; CAS mark-sweep GC; S3 small-object latency.
- Frozen gates: private dogfood **L3** (mysql+s3+sweeper+scrub); perf claims **L4** (bundle-uri); HA **L5** (daemon+multiplex).
- Strategic honesty: industry ODB still mostly block storage — differentiator engine, not a one-week Gitaly replacement.
- **Verification bar (user):** unmodified `git` only; `git clone https://localhost…`; full branch/commit/rebase/push workflows; benchmark repo [`git/git`](https://github.com/git/git); clone p50 **≤ 1.5× GitHub** and **≤ 1.5× same-stack disk git**. Program acceptance = **L4** ([005](005-stock-client-verification-and-benchmark.md)).
- Perf implication: L4 **requires** precomputed pack/bundle-uri + bitmap-class accel + warm daemon path; naïve per-oid S3 pack generation cannot pass.
- **Local test stack:** Docker Compose with **MySQL + MinIO (S3) + optional Valkey + git HTTPS** ([006](006-local-docker-compose-test-environment.md)). All integration tests and reproducible benchmarks run locally; no AWS required for merge gates. CI: compose smoke; release bench host: add GitHub clone comparison.
- **Wave 3** ([007](007-ten-agent-adversarial-panel.md)): historical in-process panel (51–70).  
- **Wave 4 authoritative** ([008](008-grok45-ten-agent-panel.md)): 10× **grok-4.5** agents (71–90). Consensus: architecture direction right; L4 keep; **6-month ship uncertain** (storage No-Go vs impl conditional Go). Hard adds: dual-write staging/chaos; sealed packs + **clone leases**; experimental not distro-default; pack DoS limits; bench anti-fraud; interop git versions; dogfood hammer/soak; cost SLIs; week-1 spikes.
- **Incremental push harness** ([009](009-incremental-push-stress-harness.md)): from **empty** cloud repo, replay `git/git` history with stock `git push` in tunable batches (1 / ramp / N); stresses receive-pack/quarantine/ref CAS, not only clone or mirror-import. CI: smoke 20× batch=1; soak profiles optional.
- **Wave 5** ([010](010-wave5-adversarial-panel.md)): 8× grok-4.5 agents. Dual-write under push-stress = top risk; add **push epoch fencing**, harness fault inject + recon + anti-cheat; **sequence** sealed packs after L2/L3 push correctness; soft batch=1 latency budgets; compose resource limits; clearer PoC done definition.

### PoC “done” means (Wave 5 product)

- Stock `git clone` / `git push` to localhost (shared test password OK)  
- Push-stress from empty repo survives smoke/ramp/chaos without corruption  
- Clone SLO on git/git ≤1.5× disk (+ GH on bench host)  
- Restart-safe; simple Compose bring-up  
- **Not** a multi-tenant auth product  

### L4 go/no-go checklist (W4-12 + W5) — PoC technical exit

Claim **PoC L4** only if **all** true (no tenancy/auth product gates):

- [ ] L2 kill-9 / mid-push chaos + **epoch compensate** green (no tip without blob)  
- [ ] Push-stress smoke + ramp + chaos (+ fault_inject) from empty repo ([009](009-incremental-push-stress-harness.md))  
- [ ] Post-batch recon asserts green  
- [ ] L3 sweeper respects tips∪pins∪packs∪**clone leases**  
- [ ] Long-lived git-http (no CGI stampede under 50 conn)  
- [ ] p50 clone = sealed pack stream (no SQL-per-oid); fallback circuit-broken  
- [ ] `p50(T_us)≤1.5 p50(T_disk)` + GitHub gate on bench host  
- [ ] `p50(T_fetch_us)≤2.0 p50(T_fetch_disk)` after push  
- [ ] P0+P1 including shallow; atomic push; interop 2.39–2.45  
- [ ] Bench JSON complete (raw trials, compose_hash, seed manifest); CV≤15%  
- [ ] 50-conn hammer + ≥7-day soak without corruption (single PoC / shared creds OK)  
- [ ] Explicit **experimental/PoC**; gc/repack safe-fail; export-to-bare exists  
- [ ] No claim of upstream/distro default; **no claim of multi-tenant secure forge**  

## Scope posture

**This is an experiment / proof of concept**, not a multi-tenant production forge product.

| In scope (PoC) | Out of scope |
|----------------|--------------|
| MySQL + MinIO/S3 storage backends | Tenancy (orgs, quotas, billing) |
| Stock git over local smart HTTP | Auth product (SSO, OAuth, ACLs) |
| Compose test/bench harness | Forge UI / PR product |
| Technical L4 clone SLO on git/git | “Secure multi-tenant SaaS” claims |

## Out of Scope (program-wide)

- **Tenancy** (orgs, per-tenant isolation products, quotas, billing, cross-tenant admin)
- **Authn/Authz product** (SSO, OAuth, fine-grained repo ACLs, forge permissions) — compose may use trivial shared basic auth **only** so push tests work; not a security product
- Bonsai canonical rewrite in v1
- Laptop cloud working-copy product
- Full forge UI
- Production multi-tenant HA harding as a ship gate (L5 aspirational only)
- Production parity claims without L4 git/git SLO evidence
- Any success path that requires a custom client binary/remote helper for clone/push
- Requiring real AWS/RDS credentials to run tests or claim verification
- Claiming L4 without W4-12 checklist  
- Shipping as non-experimental distro default while files-downcast debt remains  
