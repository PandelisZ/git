# Roadmap Status

## Active PRDs

| ID | Title | Status | Stage | Updated | Notes | Next |
|----|-------|--------|-------|---------|-------|------|
| 001 | [Vex Git Forge Storage (PoC)](001-cloud-scale-git-forge-storage.md) | Active | 4 + L3 safety foundation | 2026-08-05 | P0/P1, 20× smoke, 500-commit ramp, promotion-fault compensation, S3 scrub, pin-aware mark planning, and bounded readers are green. | Hard-kill chaos, destructive grace-period sweep with pack/lease roots, then pack work |
| 002 | [Comparative Research Digest](002-comparative-research-digest.md) | Draft | 0 - Research | 2026-08-05 | Prior art → constraints C1–C10 / M1–M4 | Add citations as found |
| 003 | [Adversarial Review Log](003-adversarial-review-log.md) | Draft | 0 - Review | 2026-08-05 | **90** passes; wave 4 grok-4.5 agents 71–90 | New passes only on design changes |
| 004 | [Vex Stage Playbooks](004-stage-playbooks.md) | Active | 4 + L3 safety foundation | 2026-08-05 | Compose Vex HTTP uses active MySQL metadata plus S3 bytes; ramp, promotion fault, scrub, pins, and bounded reader proof are verified. | Hard-kill chaos, deletion/leases, then pack work |
| 005 | [Stock Client Verification & Benchmark](005-stock-client-verification-and-benchmark.md) | Draft | 0 - Verification design | 2026-08-05 | Stock git only; git/git clone ≤1.5× GitHub and ≤1.5× disk control | Wire harness on Compose in Stage 7 |
| 006 | [Vex Local Docker Compose Test Environment](006-local-docker-compose-test-environment.md) | Active | MySQL + S3 Vex data plane | 2026-08-05 | `vex-git-http-1`, `vex-mysql-1`, and `vex-minio-1` are running; P0/P1, ramp, promotion fault, scrub, pins, and bounded readers verify the split. | Add hard-kill chaos and CI cleanup job |
| 007 | [Ten-Agent Adversarial Panel](007-ten-agent-adversarial-panel.md) | Superseded | Wave 3 | 2026-08-05 | In-process simulation (pre-grok routing) | Historical; see 008 |
| 008 | [Grok-4.5 Ten-Agent Panel](008-grok45-ten-agent-panel.md) | Complete | Wave 4 | 2026-08-05 | **Authoritative** 10 agents on grok-4.5; W4-1…W4-12; passes 71–90 | Execute week 1–2 spikes; L4 go/no-go checklist |
| 009 | [Incremental Push Stress Harness](009-incremental-push-stress-harness.md) | Active | L2 ramp + fault + bounded readers verified | 2026-08-05 | Registered 500-commit ramp passed 20/20 with reconciliation (p50 13706.237ms, p90 23252.034ms, p99 28033.833ms); promotion fault and 8-reader gate also passed. | Add hard-kill chaos and larger/soak profiles |
| 010 | [Wave 5 Adversarial Panel](010-wave5-adversarial-panel.md) | Complete | Wave 5 | 2026-08-05 | 8× grok-4.5 agents; W5-1…W5-14; push-stress + dual-write focus | Epoch fencing; sequence packs after push correctness |

## Latest Evidence Notes

### 2026-08-05

- Git insertion points: `odb/source.h` callbacks, `refs/refs-internal.h` `ref_storage_be`, `extensions.refstorage` URI payloads in `setup.c`, `odb/source-inmemory.c` as non-files template.
- ~59 `odb_source_files_downcast` call sites (gc/repack/midx/bitmap/commit-graph/http) — Vex mode cannot share those paths blindly.
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
- **Incremental push harness** ([009](009-incremental-push-stress-harness.md)): from an **empty** Vex repo, replay `git/git` history with stock `git push` in tunable batches (1 / ramp / N); stresses receive-pack/quarantine/ref CAS, not only clone or mirror-import. CI: smoke 20× batch=1; soak profiles optional.
- **Wave 5** ([010](010-wave5-adversarial-panel.md)): 8× grok-4.5 agents. Dual-write under push-stress = top risk; add **push epoch fencing**, harness fault inject + recon + anti-cheat; **sequence** sealed packs after L2/L3 push correctness; soft batch=1 latency budgets; compose resource limits; clearer PoC done definition.
- **Wave A implementation (2026-08-05):** Vex fs metadata/blob storage, `ODB_SOURCE_VEX`, `REF_STORAGE_FORMAT_VEX`, typed ref CAS/audit rows, and maintenance safe-fail guards are implemented. Targeted checks are green: `u-vex-store` (4/4), `t1490-vex-storage.sh` (2/2), and `t9905-vex-fs-first-write.sh` (1/1).
- **Compose boundary (2026-08-05):** local MySQL schema + MinIO bucket + TLS stock-Git bootstrap clone/fetch/push passed. Its `/healthz` reports `vex_storage_active:false`, so this is dependency/transport evidence only, not L1/L2 Vex storage evidence.
- **Stress baseline (2026-08-05):** the stock-Git harness passed deterministic planning and 20/20 batch=1 pushes with fresh-fetch/fsck reconciliation against a temporary classic bare target. It is not yet a Vex receiver benchmark.
- **Stage 3 Vex-FS transport (2026-08-05):** `t1491-vex-fs-http.sh` passes stock `/usr/bin/git` clone/fetch against the Vex-FS smart-HTTP server; the full smoke also passes stock clone/fetch/push. `t1492-vex-receive-pack.sh` and `t1493-vex-receive-epoch.sh` pass accepted and rejected receive epoch checks, including compensation of rejected objects. This is FS protocol evidence, not a hard-kill/hook-complete L1 claim.
- **Stage 4 backend drivers (2026-08-05):** the live native MySQL metadata probe passes with `tables=10`, `ref_version=2`, and `metadata_only=1` (the throwaway generation number varies by run); the signed MinIO blob probe passes its stage/promote/read/hash checks. Raw Git object payloads remain outside MySQL.
- **Stage 4 S3 central-store slice (2026-08-05):** `GIT_VEX_BLOB_BACKEND=s3` selects canonical signed MinIO bytes while Vex keeps metadata in the existing FS store. `t1494` proves the exact final key and fresh reader with no FS payload copy; `t1495` proves stock `/usr/bin/git` clone/fetch/push through the loopback Vex HTTP server; `t1496` proves accepted receive publication plus rejected-receive final/staging compensation. The bundled Compose `git-http` remains `bootstrap-classic-bare` and MySQL is not yet the active metadata store, so this is not L2 or L4.
- **Stage 4 active MySQL+S3 Compose data plane (2026-08-05):** Compose now builds Vex Git with `VEX_MYSQL=YesPlease` and starts `vex-git-http-1` as a long-lived gateway. Its health opens the real store and reports `metadata_backend:mysql`, `blob_backend:s3`, and `vex_storage_active:true`. `t1497` passes with only host `/usr/bin/git` over HTTPS: push, clone, fetch, atomic annotated tag plus branch, branch delete, and depth-one clone; it then checks ref/object rows in MySQL and canonical commit/tag keys in MinIO. This is a functional vertical slice only: no sealed-pack, p50, stress/chaos, or sweeper claim.
- **Safety/ramp expansion (2026-08-05):** `t1498-vex-push-stress-ramp-compose.sh` passed a unique-repo, stock-Git 500-commit `1,2,4,...,32` replay in 20/20 pushes with fresh fetch/`fsck` and MySQL/S3 reconciliation; its registered warm-stack measurement was p50 `13706.237ms`, p90 `23252.034ms`, p99 `28033.833ms`. `t1501` forced one S3 staged-to-final promotion failure and proved an aborted epoch with no new ref/object/claim/audit/final-or-staging bytes before a successful retry. `t1502` re-hashes every indexed published byte from MinIO/S3. `t1503` proves a durable pin protects an old sealed graph across a later push, then unpin restores mark candidates. `t1518` passed 20 writer pushes with 8 stock-Git readers (8 clones, 246 fetches, 8 fscks) and MySQL/S3 reconciliation. These are L3 foundations only: there is still no hard-kill chaos, destructive grace-period sweep, pack/lease root, sealed-pack path, or clone SLO claim.

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

The implementation is named **Vex** (`vex://` repositories, `ODB_SOURCE_VEX`,
and the `vex/` source tree). “Cloud” remains an architectural description.

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
