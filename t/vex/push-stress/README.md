# Incremental Vex Push-Stress Harness

The harness replays a source history into an empty target branch using stock
`git fetch`, `git update-ref`, `git push`, `git ls-remote`, and a fresh
`git fetch` + `git fsck --full` reconciliation repository. It does not use a
Vex metadata or blob API.

## Quick local proof

```sh
t/vex/push-stress/test-plan-determinism.sh
t/vex/push-stress/test-classic-bare.sh
```

The latter creates a temporary source and classic bare target, pushes the
smoke profile's 20 one-commit batches, verifies every fifth tip, then does a
fresh target fetch and full fsck. Its temporary directory is removed on exit.

## Run against a Vex endpoint

Create an empty target repository first, then supply its normal Git URL:

```sh
scripts/vex/push-stress.sh \
  --profile t/vex/push-stress/profiles/smoke.env \
  --source /path/to/git.git \
  --origin https://localhost:8443/stress/git-replay.git \
  --work-dir /tmp/vex-push-stress-work
```

`--source` is a read-only donor. The runner fetches its advertised source ref
once into its driver before timed target pushes; this bootstrap is recorded in
`run.json`, not counted as a target push. The target is always driven by normal
stock `git push` commands.

The default target branch is `main`. Source planning defaults to the donor's
symbolic `HEAD`, so an older source branch can still replay into `main`. Use
`--source-ref` or `--branch` to override either side.

## Profiles and configuration

Committed profiles are under `profiles/`:

- `smoke.env`: 20 batch=1 pushes.
- `ramp.env`: 1, 2, 4, …, 32-commit batches through 500 commits.
- `steady-1.env` and `steady-50.env`: longer write soaks.
- `chaos.env`: periodically kills the local `git push` child and retries a
  still-unadvanced step.
- `git-git-long.env`: mixed batches for a long first-parent replay.

All options can be passed as flags or set with `GIT_VEX_PUSH_STRESS_*`, for
example `GIT_VEX_PUSH_STRESS_MAX_COMMITS=100` or
`GIT_VEX_PUSH_STRESS_CONCURRENCY_READERS=2`. A supplied CLI flag takes
precedence over the profile. `GIT_VEX_MIRROR` can provide the default source.

Supported batch modes are `fixed`, `ramp`, `random` (seeded, deterministic),
and `list`. Planning is independent of the target and can be run alone:

```sh
scripts/vex/push-stress.sh \
  --source /path/to/git.git \
  --work-dir /tmp/vex-push-stress-work \
  --plan-only
```

## Artifacts and resume

The default artifact directory is `OUT_DIR` or `WORK_DIR/out` and contains:

- `plan.jsonl`: stable, sorted-key batch plan; its SHA-256 is checkpointed.
- `checkpoint.json`: next plan index and last server-confirmed tip.
- `metrics.jsonl`: one record for every actual `git push` child, including
  exit status, wall time, raw-object byte estimate, and post-push advertised
  tip.
- `recon.json`: final fresh-fetch/fsck result and any server reconciliation
  result.
- `summary.json`: completion counts and p50/p90/p99 push wall times.

Use `--resume` only with the same plan/source/origin/branch. It refuses to run
unless the plan hash and currently advertised tip exactly match the checkpoint.
The runner never deletes a supplied work or artifact directory; choose a new
one for a fresh run.

## Reconciliation boundary

The portable part of reconciliation proves that the advertised tip can be
freshly fetched and passes `git fsck --full`. Canonical object-byte truth is a
server responsibility: set `GIT_VEX_PUSH_STRESS_RECON_COMMAND` to an
operator-owned reconciliation command. The harness passes it the origin,
branch, expected tip, and plan hash as `GIT_VEX_PUSH_STRESS_*` environment
variables and records its exit status. That command must verify Vex's canonical
blob bytes and their metadata/ref reachability; the harness deliberately never
queries storage directly.

For a classic bare target no server command is configured, which is reported as
`not-configured` rather than a claim about a Vex backend.
