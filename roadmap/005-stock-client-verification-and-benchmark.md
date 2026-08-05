# Stock Client Verification & git/git Benchmark PRD

**Status:** Draft
**Current Stage:** 0 - Verification design
**Owner:** Codex
**Created:** 2026-08-05
**Updated:** 2026-08-05
**Related:** [001](001-cloud-scale-git-forge-storage.md), [004](004-stage-playbooks.md)

---

## Overview

### Problem

Cloud storage is useless as a forge backend if it needs a special client or is dramatically slower than GitHub on a real repository. Verification must prove:

1. **Any stock git client** talks to the server over normal URLs (HTTPS/SSH).
2. **No custom tooling** on the client for day-to-day git.
3. **Full remote workflow** works (clone/fetch/push/branch/tag collaboration; local commit/rebase/merge then push).
4. **Performance:** cold `git clone` of **https://github.com/git/git** class data is within **50% of GitHub** (≤ **1.5×** GitHub wall time) when served from our stack.

### Goals

- Freeze client-compatibility matrix and non-negotiable protocol surface.
- Freeze benchmark methodology for `git/git` (~80k+ commits).
- Map gates to readiness levels so L4 is a **hard SLO**, not a vibe check.
- Define server-side features required to hit the SLO (packs/bitmaps/bundle-uri/daemon/cache).
- Stress **incremental push/commit flow** (1-by-1 and tunable batches from empty → git/git history), not only full clone or one-shot mirror push — see [009](009-incremental-push-stress-harness.md).

### Non-goals

- Beating GitHub on every microbenchmark.
- Browser-only forge UI verification.
- **Tenancy or real auth product tests** (SSO, ACL matrices, tenant isolation suites) — PoC may use one shared push password.
- Requiring clients to enable experimental flags (may *offer* bundle-uri; clone must still pass without exotic client config where possible).
- Requiring real AWS/RDS for day-to-day tests (use **local Docker Compose** — [006](006-local-docker-compose-test-environment.md)).

---

## Compatibility contract (non-negotiable)

### Client rule

```text
ONLY SUPPORTED CLIENT = upstream `git` (and other clients speaking git protocol v0/v1/v2)
NO required custom CLI, SDK, or patched git on the client
```

Allowed on **server/operator** side: our cloud-backed git, MySQL, S3, Valkey, sealer/sweeper, reverse proxy.

Forbidden as a success path: “use `cloudctl clone`”, libgit2-only special remote, custom protocol scheme for end users.

### URL shapes that must work

| Transport | Example | Notes |
|-----------|---------|-------|
| Smart HTTPS | `git clone https://localhost:<port>/git/git.git` | Primary verification URL |
| Smart HTTP | `http://127.0.0.1:<port>/...` | dev OK |
| SSH (optional later) | `git@localhost:git/git.git` | same backend; not required for first SLO gate if HTTPS passes |

Stock discovery endpoints (HTTPS):

- `GET  $GIT_URL/info/refs?service=git-upload-pack`
- `POST $GIT_URL/git-upload-pack`
- `GET  $GIT_URL/info/refs?service=git-receive-pack`
- `POST $GIT_URL/git-receive-pack`

Implementation may use `git-http-backend` CGI/fcgi in front of cloud-backed bare repos, or an equivalent smart-HTTP speaker that shells to / links `upload-pack`/`receive-pack`. **Wire format must remain standard.**

### What “all git operations work” means

Split local vs remote (important):

| Operation | Where it runs | Server requirement |
|-----------|---------------|--------------------|
| `git clone` | client + upload-pack | full pack negotiation + object reachability from advertised refs |
| `git fetch` / `pull` | client + upload-pack | incremental packs, correct have/want |
| `git push` (ff, non-ff if allowed, delete, tags, multiple refs) | client + receive-pack | quarantine txn, multi-ref CAS, hooks optional |
| `git branch` / `checkout` / `commit` / `rebase` / `merge` / `cherry-pick` | **client local** after clone | server must have given a complete, consistent object graph |
| `git push` after rebase | receive-pack | force-with-lease / force policy configurable; objects + tip update |
| `git ls-remote` | upload-pack advertisement | tips correct |
| `git clone --branch` / tag checkout | upload-pack | tags/branches advertised |
| shallow clone (`--depth`) | upload-pack | **required for parity** with common CI (stage target) |
| partial clone (`--filter`) | upload-pack + promisor | **phase-gated**; not required for primary SLO if full clone hits SLO |
| LFS | separate | out of band unless repo uses LFS (git/git does not require LFS) |
| submodule | client fetch of other URLs | other repos also cloud-backed or external |

**Acceptance language:** After `git clone https://localhost/.../git.git`, a developer can use vanilla git exactly as against GitHub: branch, commit, rebase, merge, push, fetch, PR-style force-with-lease, tags. No server-specific commands.

### Protocol versions

- Support **protocol v2** for upload-pack (GitHub default path for modern git).
- Remain compatible with v0/v1 clients.
- Capabilities: `report-status`, `delete-refs`, `ofs-delta`, `side-band-64k`, `object-format` as applicable, thin packs as appropriate.

---

## Performance SLO (primary gate)

### Definition

Let:

- \(T_{gh}\) = cold wall-clock time of
  `git clone --progress https://github.com/git/git /tmp/gh-git-$$`
- \(T_{us}\) = cold wall-clock time of
  `git clone --progress https://localhost:<port>/git/git.git /tmp/us-git-$$`

**Pass condition (primary):**

```text
T_us <= 1.5 * T_gh
```

Interpretation of user wording *“within 50% of the performance boundary of doing a git clone from github”*: **no more than 50% slower than GitHub** (1.5×).
If product later wants the looser “at least half as fast” (2.0×), that is a conscious waiver — **not** the default.

### Benchmark repository

| Field | Value |
|-------|-------|
| Upstream | `https://github.com/git/git` |
| Scale class | ~80k+ commits (mirror/update at bench time) |
| Why | Real history, non-toy trees, standard industry mirror, self-hostable for import |

**Import:** server under test hosts a **faithful mirror** of git/git (all refs needed for default clone: HEAD, default branch, tags as GitHub advertises for anonymous clone). Exact ref set documented at import time (`ls-remote` diff empty for clone-relevant refs).

### Local environment (mandatory for tests)

All functional verification and the **reproducible** benchmark path run against **Docker Compose** on the developer/CI machine:

| Dependency | Local stand-in | Defined in |
|------------|----------------|------------|
| MySQL | `mysql:8` container | [006](006-local-docker-compose-test-environment.md) |
| S3 | **MinIO** container | 006 |
| Valkey | optional profile | 006 |
| Git HTTPS | `git-http` service on `localhost:8443` | 006 |

```sh
scripts/vex/up.sh                  # compose up --wait
scripts/vex/seed-git-git.sh        # mirror import
t/vex/compat-stock-client.sh
scripts/vex/bench-clone.sh         # SLO harness
```

- **No AWS account** required for merge gates.
- Same `meta-mysql` + `blob-s3` code paths as production (endpoint = MinIO).
- Shareable: committed `vex/docker/compose.yaml` + scripts.
- Resettable: `scripts/vex/reset.sh` → `down -v && up && seed`.

### Methodology (must be automated)

1. **Machine:** fixed bench host **or** any Docker-capable laptop; document CPU, RAM, disk, Docker version.
2. **Stack:** Compose project `gitcloud` healthy (006); pin image tags.
3. **Client:** same `git --version` for all clones; stock git only.
4. **Cold client:** new target dir each run; no existing repo.
5. **Server modes:**
   - **A. Cold process cache** (restart `git-http` container / drop LRU)
   - **B. Warm server cache** (steady-state after one warmup clone)
   **Primary SLO uses mode B (warm server, cold client).**
   **Secondary report mode A.**
6. **Network:**
   - \(T_{us}\), \(T_{disk}\): localhost → Compose HTTPS (loopback).
   - \(T_{gh}\): real network to `github.com` when `BENCH_INCLUDE_GITHUB=1` (release bench / optional nightly).
7. **Runs:** N≥5; report p50 and p95.
8. **Integrity:** `HEAD` match, `git fsck` clean.
9. **Artifact:** JSON under `t/vex/out/` or `roadmap/evidence/` (bench script).
10. **CI vs laptop:** CI **must** run disk-gate + functional; GitHub-gate required on designated bench host before claiming L4 release.

### Fairness & anti-cheat rules

| Allowed | Not allowed |
|---------|-------------|
| Bundle URI / precomputed packs on server | Shipping a pre-stuffed client object dir |
| Server SSD scratch + memory cache | Comparing localhost to GitHub without documenting RTT |
| HTTP/2, keep-alive, compression | Disabling GitHub features the stock client uses |
| Same ref advertisement as GitHub default clone | Serving a shallow/truncated “fake git.git” for the timed run |

**RTT fairness addendum:** Loopback vs `github.com` is not equal RTT. To prevent empty bragging:

- Always publish \(T_{gh}\), \(T_{us}\), and **bytes transferred**.
- Additionally run **same-LAN control**: clone from a **classic bare git/git on local disk** via same HTTPS frontend (`T_disk`).
- **Structural efficiency gate:** `T_us <= 1.5 * T_disk` on same host/frontend.
  This proves cloud backend overhead is within 50% of disk git over the same protocol stack.
- **Product gate (user-facing):** still `T_us <= 1.5 * T_gh` from the bench host.

Both gates must pass for **L4 complete**.

### Fetch-after-push gate (secondary, required for product L4)

After a small push that moves default branch (invalidating tip coverage until pack rebuild or incremental pack):

```text
p50(T_fetch_us) <= 2.0 * p50(T_fetch_disk)
```

If pack rebuild is async, either (a) rebuild completes before fetch measurement, or (b) incremental path still meets gate — document which.

### Secondary performance metrics (report, soft gates later)

| Metric | Target (initial) |
|--------|------------------|
| Incremental `git fetch` (1 new commit) | ≤ 1.5× GitHub same scenario |
| `git push` of 1 small commit | ≤ 2× local-disk HTTPS push (no GitHub push baseline) |
| `ls-remote` | ≤ 100ms p50 localhost warm |
| Second full clone warm server | not slower than first by >20% variance unexplained |

---

## Compat tiers

| Tier | Includes | L4 required? |
|------|----------|--------------|
| **P0** | ls-remote, full clone, fetch, push ff, force-with-lease, delete ref (policy), annotated tags, multi-ref atomic push, protocol v2 | **Yes** |
| **P1** | `clone --depth`, deepen/fetch deepen, unshallow | **Yes** |
| **P2** | partial clone `--filter`, promisor | No (deferred) |
| **P3** | replace refs, alternates, odd maintenance | No |

L4 acceptance = **P0+P1** on mirrored git/git + clone SLOs + fetch-after-push secondary SLO.

## Functional verification matrix (stock client)

All commands use only `git` against `ORIGIN=https://localhost:<port>/git/git.git` unless noted.

### F1 — Anonymous / read path

```sh
git ls-remote "$ORIGIN"
git clone "$ORIGIN" repo
cd repo && git fsck --full
git log -1 --oneline
git checkout -b smoke
```

### F2 — Write path (PoC shared credential only)

```sh
# Compose may expose a single test user/password for receive-pack.
# Not an auth product — just enough for stock git push.
echo x >> README && git add README && git commit -m m
git push -u origin HEAD:refs/heads/smoke-push
git push origin :refs/heads/smoke-push   # delete if allowed
```

### F3 — Branching collaboration

```sh
git fetch origin
git checkout -b feature origin/master   # or main
# commit, push feature
git push origin HEAD:refs/heads/feature
```

### F4 — Rebase + force-with-lease

```sh
git checkout feature
git rebase origin/master
git push --force-with-lease origin feature
```

### F5 — Merge commit push

```sh
git checkout master
git merge --no-ff feature
git push origin master
```

### F6 — Tags

```sh
git tag -a v-smoke -m t
git push origin v-smoke
git ls-remote --tags origin | grep v-smoke
```

### F7 — Multi-ref push

```sh
git push origin master feature
```

### F8 — Concurrent readers during push

- Reader clones/fetches while writer pushes; no corrupt pack; tips atomic.

### F9 — Shallow (**P1**, L4 required)

```sh
git clone --depth 50 "$ORIGIN" shallow
git -C shallow fetch --depth 100 origin
git -C shallow fetch --unshallow origin
```

### F10 — Protocol v2

```sh
GIT_TRACE_PACKET=1 git -c protocol.version=2 ls-remote "$ORIGIN" 2>&1 | grep -E 'ls-refs|fetch'
```

### F11 — Fetch-after-push (secondary SLO)

```sh
# clone warm, push one commit from writer, fetch on reader
# p50(T_fetch_us) <= 2.0 * p50(T_fetch_disk)
```

### F12 — Maintenance safe-fail

```sh
git -C "$bare" gc
git -C "$bare" repack -ad
git -C "$bare" multi-pack-index write || true
# must not BUG/downcast crash; may no-op with message
```

### F13 — Atomic multi-ref + push-options

```sh
git push --atomic origin master feature
git push -o confirmed=true origin master  # server records option; must not crash
```

### F14 — Interop versions

Run F1–F3 with stock git **2.39, 2.42, 2.45** (or platform equivalents) against same server.

### F15 — Concurrent hammer (dogfood)

```sh
# 50 parallel: clone/fetch/push to distinct branches for 10+ minutes
# zero corruption; tips match ref_updates
```

### F16 — Chaos mid-push

```sh
# kill -9 receive-pack between blob put and ref CAS (multiple offsets)
# assert: tip unchanged; no published index without blob; restart safe
```

### F17 — Incremental push stress (write path)

```sh
# empty cloud bare + replay source history with stock git pushes
scripts/vex/push-stress.sh --profile t/vex/push-stress/profiles/smoke.env
scripts/vex/push-stress.sh --profile t/vex/push-stress/profiles/ramp.env
# optional soak:
# scripts/vex/push-stress.sh --profile t/vex/push-stress/profiles/steady-1.env
```

See [009](009-incremental-push-stress-harness.md). Complements clone SLO (read path).

---

## Server capabilities required to hit SLO

Hitting ≤1.5× GitHub on git/git **will not** happen with naïve per-oid S3 GET pack generation. Stage mapping:

| Capability | Why | Min level |
|------------|-----|-----------|
| Smart HTTP(S) frontend | stock `git clone https://…` | L1+ |
| Multi-ref advertisement | clone | L1 |
| Streaming pack from stored objects | correctness | L1 |
| `commit_parents` / graph helpers | negotiation | L1–L2 |
| Process blob LRU + scratch | latency | L1–L2 |
| Long-lived daemon + pools | MySQL/S3 conn + CPU | L4/L5 practical |
| **Bitmap or equivalent reachability accel** | pack enumeration like GitHub | **L4 required** |
| **Precomputed pack or bundle for full clone** | avoid building 80k-commit pack from scratch every time | **L4 required** |
| Bundle-URI advertisement | stock clients can multi-source | L4 preferred |
| Delta reuse / thin packs | bandwidth | L4 |
| Warm cache of hot pack | p50 stability | L4 |

**Design implication (update to 001):** L4 is not “optional polish.” **L4 is the user acceptance bar for performance.** L1–L3 are stepping stones only.

---

## Import fixture: git/git

### One-time / refresh procedure (operator)

```sh
# classic mirror for import source
git clone --mirror https://github.com/git/git git.git
# push mirror into cloud-backed bare (once receive-pack + auth exist)
git --git-dir=git.git push --mirror https://localhost:<port>/git/git.git
# or server-side importer reading mirror packs into vex_store
```

Verification:

```sh
diff -u <(git ls-remote https://github.com/git/git | sort) \
        <(git ls-remote https://localhost:<port>/git/git.git | sort)
# allow intentional ref policy differences only if documented;
# default clone HEAD^{commit} must match.
```

### Data scale expectations (order of magnitude)

Document at import: commit count, object count, uncompressed/compressed pack size, ref count. Use these as regression canaries (object count must not drop).

---

## Automated harness sketch (Compose-backed)

All harnesses assume `scripts/vex/up.sh` already succeeded (or they call it).

`scripts/vex/bench-clone.sh` → wraps `t/vex/perf-clone-git-git.sh` (Stage 7+):

1. Resolve from env/compose: `US_URL` (cloud), `DISK_URL` (classic bare on same front), optional `GH_URL`.
2. Discard 1 warmup clone; time N≥5 (prefer ≥10) cold-client clones.
3. Emit JSON with **required fields**:
   `benchmark_id, timestamp_utc, git_commit, manifest_seed, compose_hash, n_trials, trials[], p50_wall_s, p90_wall_s, p99_wall_s, cv, baselines, ratios, env, optional_github_gate`.
4. Fail if CV>15% (unstable) or gates fail.
5. `--require-github` for full L4 release; CI uses disk gate only.
6. Publish **raw trials** (no best-of cherry-pick).

`t/vex/compat-stock-client.sh`:

1. Run F1–F8 (F9 soft) against `$GIT_VEX_ORIGIN`.
2. Fail on any non-zero.
3. Use `http.sslCAInfo` pointing at compose dev CA when HTTPS.

`t/vex/integration-mysql-minio.sh`:

1. Verify MySQL schema reachable and MinIO bucket non-empty after seed.
2. `git ls-remote` + tiny push/clone round-trip.

---

## Exit criteria by level (revised)

| Level | Compat | Perf |
|-------|--------|------|
| L1 | F1–F3 on **tiny** repo via HTTPS localhost | no git/git SLO |
| L2 | F1–F7 on medium synthetic + mysql/s3 | no git/git SLO |
| L3 | F1–F8 + crash/sweeper + **push-stress smoke/ramp/chaos** | no full clone SLO |
| **L4** | F1–F8 on git/git + push-stress steady sample | **`T_us≤1.5 T_gh` and `T_us≤1.5 T_disk`** + fetch-after-push |
| L5 | same under restart/HA drill | p95 within documented factor |

**User-facing “done” for this program = L4 minimum.**

---

## Risks to the SLO

| Risk | Mitigation |
|------|------------|
| Building full pack from loose S3 objects each clone | precomputed pack/bundle + bitmaps |
| MySQL RTT on every object exists | bulk bitmap / pack index in memory; not per-oid SQL in hot pack path |
| Short-lived `git-http-backend` process | long-lived daemon |
| Delta quality worse than GitHub | store/re-serve high-quality packs; don’t recompress live poorly |
| Fairness complaints (localhost vs GH) | dual gate with `T_disk` |
| Import incomplete | ls-remote + fsck + HEAD match |

---

## Evidence Log

### 2026-08-05

- User verification criteria captured: stock client only; full workflows; ≤50% of GitHub clone boundary; benchmark `github.com/git/git`.
- Protocol surface: smart HTTP upload-pack/receive-pack (upstream docs).
- Industry: GitHub clone performance depends on packed representation + edge caching — forces L4 pack/bundle/bitmap work as acceptance, not stretch.
- **Local Docker Compose** mandated for MySQL + MinIO (+ Valkey) tests/benchmarks (user follow-up; PRD 006).

## Open Questions

- [ ] SSH in L4 gate or HTTPS-only? Recommendation: **HTTPS required**, SSH soon after.
- [ ] Auth mode for push tests (basic, mTLS, unix socket)? Recommendation for compose: **HTTP basic** or push over loopback with test hook auth.
- [ ] Exact default branch name at import time (`master` vs `main`) follow GitHub HEAD.
- [ ] Whether protocol v2 + bundle-uri may be enabled by default on server advertisement.
