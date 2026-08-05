# Wave A3 spike evidence

## Status

The local harness spike is **pass**. It proves deterministic planning and the
stock-Git execution/reconciliation path against a classic bare target. It does
not prove Vex storage, epoch fencing, or performance readiness.

## Commands run — 2026-08-05

```sh
git --version
t/vex/push-stress/test-plan-determinism.sh
t/vex/push-stress/test-classic-bare.sh
```

Observed client: `git version 2.50.1 (Apple Git-155)`.

| Spike | Result | Evidence |
|---|---|---|
| Deterministic `plan.jsonl` | PASS | Two independent seeded random plans for the same 7-commit fixture compared byte-for-byte. |
| Empty-target push smoke | PASS | 20/20 batch=1 stock `git push` operations accepted by a temporary classic bare repository. |
| Checkpoint/metrics/recon artifacts | PASS | Run emitted a SHA-256-bound checkpoint, 20 metrics records with `push_exit`, summary, and a final fresh-fetch/fsck recon. |
| Final reachability | PASS | Final advertised tip matched the final plan tip; a fresh fetch followed by `git fsck --full` passed. |

The smoke run reported p50/p90/p99 push wall times of `47.882` / `50.873` /
`53.076` ms for this tiny local fixture. These are harness observations, not a
latency target or a Vex performance claim.

## Manageable disk-clone baseline

The same self-contained fixture has 24 empty commits. A local
`git clone --no-checkout` from its classic bare source took `59.107 ms` in the
successful run above. This is only a repeatable harness baseline.

No local `git/git` mirror was present in this worktree (`find . -maxdepth 4
-type d -name git.git` returned no result), so no `git/git` fetch or clone
baseline was attempted during this spike. It must be measured later on the
designated benchmark host and cannot support an L4 claim.

## Completion — 2026-08-05

The remaining Wave A3 informational measurements were run after the harness
foundation landed:

| Spike | Result | Measurement |
|---|---|---|
| Naive pack + bitmap | PASS | A synthetic bare repo with 10,002 objects packed with `git repack -adb --write-bitmap-index` in **0.03 s**. It produced one **679.95 KiB** pack and its matching bitmap. |
| `git/git` source fixture | PASS | A fresh public mirror resolved to `5b2471720c93ee30e5764a19f3d3b3ae9ec9712a` with **846,402** packed objects and a **590.60 MiB** pack. |
| Disk clone control | PASS (informational) | Default local clone took **0.10 s** because it may hardlink. The meaningful `git clone --no-local --no-checkout` transfer control took **15.03 s** on this machine. |

These are single-run, local-machine measurements. They establish a reproducible
fixture and confirm bitmap generation; they are not a GitHub comparison, an L4
SLO result, or a Vex serving benchmark.

## Canonical-byte reconciliation

The harness intentionally makes no metadata or object-store calls. Its portable
recon is fresh stock-Git fetch + `git fsck --full`. When the Vex backend is
available, `GIT_VEX_PUSH_STRESS_RECON_COMMAND` must run the server-owned recon
that verifies canonical blob bytes (including OID/hash truth), metadata, and
tip reachability. The classic-bare smoke correctly reports this backend-specific
check as `not-configured`.
