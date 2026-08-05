# Implementation log

## 2026-08-05 — Wave A3 / early write-path harness

Shipped a stock-Git incremental push-stress harness under `scripts/vex/` and
`t/vex/push-stress/`.

- Deterministic first-parent, topo, explicit, fixed, ramp, seeded-random, and
  list batch planning writes `plan.jsonl`.
- The driver pushes each planned tip with ordinary `git push`; each actual child
  attempt is recorded in `metrics.jsonl`.
- Strict checkpoint/resume validation binds plan SHA-256, source, origin,
  branch, and advertised server tip.
- Final recon makes a fresh target fetch and runs `git fsck --full`; an optional
  server-owned command records canonical-byte reconciliation without the
  harness directly accessing storage.
- Smoke, ramp, steady, chaos, and long-run profiles are committed. The smoke
  profile was run successfully against a temporary classic bare repository.

Commands and result:

```sh
t/vex/push-stress/test-plan-determinism.sh
# PASS: deterministic plan.jsonl

t/vex/push-stress/test-classic-bare.sh
# PASS: 20/20 batch=1 pushes, final fresh-fetch/fsck recon true
```

Current blocker for backend-specific acceptance: the Vex object/metadata and
smart-HTTP implementation is not yet available to this harness. In particular,
this evidence does not claim epoch compensation, canonical blob durability, or
any clone/push SLO.
