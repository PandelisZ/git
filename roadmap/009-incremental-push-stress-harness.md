# Incremental Push Stress Harness PRD

**Status:** Active
**Current Stage:** Harness foundation — Vex-FS target not yet wired
**Owner:** Codex
**Created:** 2026-08-05
**Updated:** 2026-08-05
**Related:** [001](001-cloud-scale-git-forge-storage.md), [004](004-stage-playbooks.md), [005](005-stock-client-verification-and-benchmark.md), [006](006-local-docker-compose-test-environment.md)

---

## Overview

### Problem

Current verification overweights:

1. **Cold full clone** (read path / pack serve)
2. **One-shot mirror push** (`git push --mirror` of entire history)

Real forge load is mostly **many small receive-pack transactions**: 1 commit, a few commits, a branch tip move, tags, force-with-lease. A system can pass clone SLO and still die on:

- quarantine open/commit/abort churn
- dual-write staging promote per push
- ref CAS + `ref_updates` write amp
- pack invalidation / debounced rebuild thrash
- commit_parents extraction latency
- MySQL connection behavior under thousands of short pushes

### Goals

1. Start from an **empty** Vex-backed bare repo.
2. Replay history from a **source fixture** (default: local mirror of `github.com/git/git`) by pushing commits **1-by-1 or in tunable batches**.
3. Use **stock `git` only** on the wire (no custom push protocol).
4. Produce **shareable, resumable, metric-rich** runs on Docker Compose.
5. Stress **commit/push flow**, not only clone/mirror-import.
6. Remain PoC-scoped: no tenancy/auth product (shared compose push password OK).

### Non-goals

- Replacing the L4 clone SLO (complementary).
- Perfect recreation of GitHub’s exact push traffic mix.
- Multi-tenant concurrent auth matrices.
- Reimplementing `git` pack negotiation in the harness (shell out to git).

### Implemented boundary (2026-08-05)

The plan/run/verify implementation and smoke/ramp/chaos profiles are
committed. `test-plan-determinism.sh` and `test-classic-bare.sh` passed; the
latter performed 20/20 stock-Git batch=1 pushes with a fresh-fetch and
`git fsck --full` reconciliation.

That target is intentionally a temporary classic bare repository. The same
smoke remains a required gate against the Vex receiver, and this baseline does
not claim epoch compensation, blob durability, or a Vex performance number.

A separate Vex-FS smart-HTTP smoke now proves stock-Git clone/fetch/push and
accepted/rejected receive epochs. The stress harness has not yet targeted that
endpoint, so its classic-bare result remains only a baseline.

---

## Core idea

```text
SOURCE (read-only)              HARNESS                         TARGET (Vex bare)
.git mirror of git/git    →   plan commit batches      →    empty ORIGIN via receive-pack
                              update local tip clone
                              git push stock client
                              record metrics / checkpoint
```

The harness never speaks MySQL/S3. It only:

1. Reads source history with stock git.
2. Maintains a **driver work clone** bound to `ORIGIN`.
3. Advances `refs/heads/main` (or configured branch) by **fetching objects from source** into the driver and **pushing** to ORIGIN.
4. Optionally runs concurrent readers (clone/fetch) against ORIGIN.

---

## Components

### Layout (implement later)

```text
scripts/vex/
  push-stress.sh              # CLI entry
t/vex/push-stress/
  README.md
  plan.py                     # or plan.sh — build batch plan from source
  run.sh                      # execute plan
  verify.sh                   # post-conditions
  lib/
    git_helpers.sh
    metrics.sh
  profiles/
    smoke.env                 # batch=1, max_commits=20
    ramp.env                  # 1, then 2,4,8,...
    steady-8.env              # batch=8 through first N commits
    git-git-1by1.env          # batch=1, first-parent full (long)
    git-git-batches.env       # batch=50 first-parent
  out/                        # gitignored results
    plan.jsonl
    checkpoint.json
    metrics.jsonl
    summary.json
```

### CLI surface

```sh
scripts/vex/push-stress.sh \
  --source /mirror/git.git \
  --origin https://localhost:8443/stress/git-replay.git \
  --profile profiles/ramp.env \
  --work-dir /tmp/push-stress-work \
  --max-commits 5000 \
  --batch-size 1 \
  --batch-mode fixed|ramp|random|list \
  --order first-parent|topo \
  --branch main \
  --resume \
  --concurrency-readers 0 \
  --pause-ms 0 \
  --kill-every 0 \
  --verify-every 50
```

All knobs also via env file (`PUSH_STRESS_*`).

---

## History planning

### Source selection

| Input | Default |
|-------|---------|
| `--source` | `$VEX_GIT_MIRROR` or `vex/docker/volumes/git-mirror/git.git` |
| Tip | `source/HEAD` → default branch |
| Object set | commits along chosen order, each with full tree closure needed for push |

### Order modes

1. **`first-parent`** (default for long runs)
   - Walk `rev-list --first-parent --reverse HEAD`
   - Simulates “mainline landing” stream
   - Cheaper, still ~tens of thousands of steps on git/git

2. **`topo`**
   - `rev-list --topo-order --reverse HEAD`
   - Includes merge commits with side history as dependencies appear
   - Heavier; better coverage of merge pushes

3. **`explicit`**
   - File of OIDs or refs (for tiny synthetic fixtures)

### Batching modes

| Mode | Behavior |
|------|----------|
| `fixed` | Every push advances exactly `B` commits (`--batch-size B`) |
| `ramp` | Batches `1,2,4,...,Bmax` then repeat or cap |
| `random` | Uniform between `Bmin` and `Bmax` each step |
| `list` | Comma list e.g. `1,1,1,10,50,1` cycling |
| `bytes-cap` | Grow batch until estimated new objects exceed `--max-objects-per-push` (optional advanced) |

**Batch = 1** is the primary stress for quarantine/ref CAS.
**Batch = 50–200** stresses medium receive-pack.
**Occasional large batch** (e.g. 1000) optional via list mode — still not full-mirror unless configured.

### Plan artifact (`plan.jsonl`)

One line per push step (generated before run, or streamed):

```json
{"i":0,"batch":1,"want_tip":"<oid>","base_tip":"<oid>|null","n_commits":1,"commit_oids":["..."],"mode":"first-parent"}
{"i":1,"batch":2,"want_tip":"<oid>","base_tip":"<oid>","n_commits":2,"commit_oids":["...","..."]}
```

Planning is pure local git; no server load. Re-runnable / diffable.

---

## Execution algorithm (stock git)

### Bootstrap

```sh
# TARGET empty Vex bare already exists (seed script)
git init --bare  # server-side empty Vex repo

# DRIVER
rm -rf "$WORK"
git clone --no-checkout "$ORIGIN" "$WORK"   # may be empty unborn HEAD
# add source as object donor
git -C "$WORK" remote add src "$SOURCE"
git -C "$WORK" fetch src --filter=blob:none   # or full fetch once into driver
# simpler reliable approach for PoC:
```

### Recommended reliable driver method (PoC)

Avoid clever partial clones for the harness itself:

1. **Once:** `git clone --mirror $SOURCE_URL /mirror/git.git` (or use existing).
2. **Driver repo** starts empty, `origin` = cloud ORIGIN.
3. For each plan step with `want_tip`:

```sh
# Ensure driver has objects: fetch from source by OID
git -C "$WORK" fetch src "$want_tip"

# Point local branch at want_tip without checkout cost when possible
git -C "$WORK" update-ref refs/heads/$BRANCH "$want_tip"

# Push only that ref (stock git builds thin/thick pack as needed)
git -C "$WORK" push origin "refs/heads/$BRANCH:$refs/heads/$BRANCH"
```

For **batch size 1**, each push is one new commit (+ trees/blobs not yet on server).
For **batch size N**, tip jumps N first-parent commits; one receive-pack sends all missing objects.

**Empty → first push:** allow unborn → first commit (`--set-upstream`).

### Why not `git push src` directly?

Harness must use ORIGIN as the cloud server under test; source is local disk donor only.

### Alternate implementation: bundle slices

For each batch, `git bundle create step.bundle $base..$want` then `git fetch bundle && git push`.
More moving parts; **OID fetch + update-ref + push** is enough for PoC.

---

## Tunables (full list)

| Knob | Default | Purpose |
|------|---------|---------|
| `batch_size` | 1 | Commits per push |
| `batch_mode` | fixed | fixed/ramp/random/list |
| `batch_max` | 64 | Ramp/random cap |
| `order` | first-parent | History walk |
| `max_commits` | 0=all | Cap for smoke |
| `max_pushes` | 0=all | Cap steps |
| `pause_ms` | 0 | Inter-push delay (rate control) |
| `verify_every` | 50 | ls-remote + rev-parse tip match |
| `fsck_every` | 0 | expensive; 0=off or end only |
| `concurrency_readers` | 0 | Background `git fetch` loops |
| `reader_interval_ms` | 1000 | Reader pace |
| `kill_every` | 0 | Every K pushes, SIGKILL mid-push (chaos) |
| `force_with_lease_every` | 0 | Rewrite last commit and lease-push |
| `tag_every` | 0 | Annotated tag + push |
| `side_branch_every` | 0 | Push tip to `stress/branch-i` too |
| `resume` | false | Continue from checkpoint |
| `fail_fast` | true | Stop on first push error |

---

## Checkpoint & resume

`checkpoint.json`:

```json
{
  "plan_sha256": "...",
  "next_i": 120,
  "last_ok_tip": "<oid>",
  "origin": "https://localhost:8443/stress/git-replay.git",
  "source": "/mirror/git.git",
  "started_at": "...",
  "updated_at": "..."
}
```

On `--resume`, skip plan lines `i < next_i`, confirm `origin/BRANCH == last_ok_tip` (or reset driver to match server tip).

---

## Metrics (per push → `metrics.jsonl`)

```json
{
  "i": 12,
  "t_start": 0,
  "t_end": 0,
  "dt_ms": 0,
  "batch": 1,
  "n_commits": 1,
  "base_tip": "...",
  "want_tip": "...",
  "push_exit": 0,
  "bytes_sent_est": 0,
  "phase_fetch_src_ms": 0,
  "phase_push_ms": 0,
  "server_tip_after": "...",
  "error": null
}
```

Optional with `GIT_TRACE2_EVENT` parse:

- `receive-pack` wall on server if logs mounted
- objects offered/negotiated (best-effort from trace)

### Summary (`summary.json`)

- total pushes, commits, wall time
- p50/p90/p99 `dt_ms` overall and by batch size
- failures, retries
- commits/s, pushes/s
- tip match final `origin` vs plan last want
- optional: clone time of ORIGIN after run (links to read-path after write stress)

---

## Verification after steps / end

### Cheap (every `verify_every`)

```sh
srv=$(git ls-remote "$ORIGIN" refs/heads/$BRANCH | awk '{print $1}')
test "$srv" = "$want_tip"
```

### Medium (end or periodic)

```sh
git clone --depth 1 "$ORIGIN" /tmp/v
git -C /tmp/v fetch --depth 50
# or full clone on short runs
```

### Expensive (end of profile or nightly)

```sh
git clone "$ORIGIN" /tmp/full
git -C /tmp/full fsck --full
# compare rev-list count to plan
```

### Invariant checklist

| ID | Invariant |
|----|-----------|
| I1 | Every successful push moves tip to `want_tip` |
| I2 | Failed push leaves tip at previous `last_ok_tip` |
| I3 | Kill mid-push never advances tip without objects (chaos mode) |
| I4 | Final first-parent count ≥ planned commits |
| I5 | No harness use of mysql/s3 APIs |
| I6 | Before first push: ORIGIN has **no** commits on branch (empty/unborn) |
| I7 | Every `verify_every`: tip match + optional `git fsck --full` on a fresh clone |
| I8 | Checkpoint resume only if `ls-remote` tip == `last_ok_tip` and `plan_sha256` matches |
| I9 | Each metrics line corresponds to a real `git push` attempt (exit code recorded; anti-cheat) |

---

## Profiles (committed)

### `smoke.env` — minutes

```env
PUSH_STRESS_ORDER=first-parent
PUSH_STRESS_BATCH_MODE=fixed
PUSH_STRESS_BATCH_SIZE=1
PUSH_STRESS_MAX_COMMITS=20
PUSH_STRESS_VERIFY_EVERY=5
```

### `ramp.env` — quarantine + medium packs

```env
PUSH_STRESS_BATCH_MODE=ramp
PUSH_STRESS_BATCH_SIZE=1
PUSH_STRESS_BATCH_MAX=32
PUSH_STRESS_MAX_COMMITS=500
```

### `steady-1.env` — death by a thousand pushes

```env
PUSH_STRESS_BATCH_MODE=fixed
PUSH_STRESS_BATCH_SIZE=1
PUSH_STRESS_MAX_COMMITS=5000
PUSH_STRESS_CONCURRENCY_READERS=2
```

### `steady-50.env` — grouped landings

```env
PUSH_STRESS_BATCH_SIZE=50
PUSH_STRESS_MAX_COMMITS=20000
```

### `chaos.env`

```env
PUSH_STRESS_BATCH_SIZE=1
PUSH_STRESS_MAX_COMMITS=200
PUSH_STRESS_KILL_EVERY=15
PUSH_STRESS_FAIL_FAST=0
```

### `git-git-long.env` — overnight PoC

```env
PUSH_STRESS_ORDER=first-parent
PUSH_STRESS_BATCH_MODE=list
PUSH_STRESS_BATCH_LIST=1,1,1,1,1,8,8,32,1
PUSH_STRESS_MAX_COMMITS=0
PUSH_STRESS_VERIFY_EVERY=100
PUSH_STRESS_READERS=4
```

---

## Compose integration

```sh
scripts/vex/up.sh
scripts/vex/seed-empty-repo.sh stress/git-replay   # empty Vex bare
# mirror once if needed
scripts/vex/ensure-git-git-mirror.sh

scripts/vex/push-stress.sh --profile t/vex/push-stress/profiles/smoke.env

# after write stress, optional read SLO sample
git clone https://localhost:8443/stress/git-replay.git /tmp/after
```

Empty repo helper creates Vex bare with HEAD→main unborn or empty tree policy documented.

---

## What this stresses on the server (map to design)

| Harness behavior | Server subsystem |
|------------------|------------------|
| batch=1 × thousands | receive-pack, odb_transaction, quarantine gens, ref CAS, ref_updates amp |
| ramp batches | pack window sizing, object fan-in per txn |
| concurrent readers | upload-pack during invalidation; clone leases |
| kill_every | dual-write compensate; tip atomicity |
| long run | sweeper vs live tips; pack rebuild debounce backlog |
| final clone | pack regenerate quality after many small pushes |

---

## Gates (when harness exists)

| Gate | Level | Criterion |
|------|-------|-----------|
| G-smoke | L2+ | `smoke.env` 20× batch=1 green |
| G-ramp | L3 | `ramp.env` 500 commits green; tip match |
| G-chaos | L3 | `chaos.env` no tip/object split brain |
| G-steady1 | L3/L4 | 5k× batch=1; p99 push latency budget TBD on hardware |
| G-post-clone | L4 related | After 5k pushes, clone still works; optional vs disk control |
| G-long | nightly | first-parent toward full git/git with mixed batches |

**Note:** Completing full git/git 1-by-1 (~80k pushes) is a **soak**, not a PR gate. PR gate = smoke+ramp+chaos.

---

## Latency budgets (initial soft targets, local Compose)

Document hardware. Soft only until calibrated (Wave 5 perf agent):

| batch | p50 push | p99 push |
|-------|----------|----------|
| 1 | ≤ 120–200ms | ≤ 400–1000ms |
| 8 | &lt; 500ms | &lt; 2s |
| 50 | &lt; 2s | &lt; 5s |

**Not** L4 hard gates; L4 hard gates remain clone SLOs. Push budgets catch receive-pack regressions under batch=1 storms.

### Required metrics fields (extra)

`dt_ms`, `push_exit`, `batch`, `n_commits`, `want_tip`, `server_tip_after`, plus optional host: CPU%, RSS, and server log push-id if available.

---

## Failure injection modes

1. **`kill_every`** — SIGKILL driver `git push` child mid-transfer
2. **`pause_ms` + restart server container** every N pushes
3. **`network_delay`** (optional `tc` in compose profile)
4. **Reader stampede** — 20 fetchers during batch=1 stream
5. **`fault_inject`** (server-side or proxy, Wave 5): after N pushes, force MinIO 503 / MySQL rollback / delay between blob promote and ref CAS — harness asserts tip unchanged and recon clean
6. **`concurrent_drivers`** — K parallel push-stress workers on **different branches** (optional profile; not default CI)

### Anti-cheat (W5-5)

- `summary.json` must embed `plan_sha256` and count of metrics lines with `push_exit` present.
- Resume refused if server tip ≠ checkpoint.
- CI fails if `dt_ms` all zeros or pushes &lt; plan steps without errors.

---

## Implementation notes (when coding)

- Language: `bash` + `python3` for plan/metrics is fine; keep push as subprocess `git`.
- Do not parse packfiles in harness.
- Prefer `GIT_TRACE2_EVENT` to files per step for debug profiles.
- Ensure `http.sslCAInfo` set for compose TLS like other tests.
- Windows: out of scope for harness v1 (Linux/macOS).

---

## Stage mapping

| Stage | Harness work |
|-------|----------------|
| **3 (fs protocol)** | **Land harness early (W5-14)**; smoke.env as soon as receive-pack works |
| **4 (compose mysql+minio)** | smoke + ramp + chaos + fault_inject; **L2/L3 do not require sealed packs** (W5-4) |
| 6 | steady-1 soak; concurrent readers; sweeper under write load |
| 7 | post-push-stress **clone** sample; pack debounce under write load; fetch-after-push |

---

## Acceptance (design → impl)

- [ ] Plan from git/git first-parent produces deterministic `plan.jsonl`
- [ ] Empty ORIGIN + batch=1 pushes 20 commits; tip matches
- [ ] batch=50 jumps tip by 50; one receive-pack per step
- [ ] resume works after Ctrl-C
- [ ] chaos kill does not publish tip without objects
- [ ] metrics.jsonl + summary.json emitted
- [ ] Documented in `t/vex/push-stress/README.md`
- [ ] Wired in 005 as write-path stress alongside clone SLO

---

## Evidence Log

### 2026-08-05

- User request: harness from empty repo; push 1-by-1 or tunable batches from git/git; stress commit/push flow not only clone/big push.
- Complements 005 clone SLO and mirror import path.

## Open Questions

- [ ] Default branch name when replaying ancient git/git (`master` vs `main`) — follow source HEAD
- [ ] Whether topo-order merge batches need special “push merge with parents already present” asserts (yes if topo enabled)
- [ ] Soft latency budgets calibration hardware
