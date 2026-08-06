# Vex test harnesses

`push-stress/` exercises an empty target repository through unmodified Git
transport commands. It is usable against a classic bare repository today and
can now target the Compose Vex smart-HTTP endpoint as the write-path gate.

Run the self-contained local proof:

```sh
t/vex/push-stress/test-plan-determinism.sh
t/vex/push-stress/test-classic-bare.sh
```

See [push-stress/README.md](push-stress/README.md) for profiles, artifacts, and
the server-owned canonical-byte reconciliation hook.

## MySQL + S3 Compose smart-HTTP gate

`mysql-s3-stock-http.sh` brings up the source-built Compose server, requires
an active MySQL+S3 health payload, then uses only an unmodified host Git client
to push, clone, fetch, atomically publish an annotated tag plus branch, delete
the transient branch, run a depth-one clone, and fsck a fresh thin Vex
repository over HTTPS. It is the data-plane E2E gate; it does not claim the
sealed-pack clone SLO.

```sh
t/vex/mysql-s3-stock-http.sh
VEX_TEST_COMPOSE_MYSQL_S3=1 make -C t T=t1497-vex-compose-mysql-s3.sh
VEX_TEST_COMPOSE_MYSQL_S3=1 make -C t T=t1524-vex-compose-mysql-s3-hard-kill.sh
```

## git/git seed and disk control

For the real-repository read-path fixture, first start the active local Vex
stack, then seed it with normal stock-Git HTTPS receive-pack traffic:

```sh
scripts/vex/up.sh
scripts/vex/seed-git-git.sh
```

The seed script caches a host-side `--mirror` clone under
`t/vex/out/git-git-mirror.git`, imports it into `git/git.git`, proves the
sealed immutable Vex S3 pack and refs/HEAD, then creates a separate
`t/vex/out/git-git-disk.git` control using
`git repack -adf --write-bitmap-index` and writes a redacted
`t/vex/out/seed-manifest.json`. It reports phase timings and fails the
server-only `raw_ingest_seconds` gate if the interval from immediately before
the stock `push --mirror` through seal and availability verification exceeds
five minutes; disk-control and manifest work are outside that gate. Use
`--refresh` to update the cached mirror. The first-pack workflow accepts only
an empty Vex target or one whose refs already match the mirror; it does not
replace a different non-empty target. It never starts Compose itself.
When an older local `.env` caps smart-HTTP bodies below 8 GiB, it temporarily
recreates only `git-http` at that admission limit for the mirror push, verifies
the active data plane, then restores the gateway's previous running limit.

These artifacts make the full-sized fixture reproducible; they do not by
themselves satisfy the L4 latency or direct-pack-serving gates.

## Benchmark evidence

`scripts/vex/bench-clone.sh` is an explicit-input evidence runner. It does not
start or alter Compose while timing; it records source/Compose identity,
every raw warmup and trial, clone/fetch `fsck` results, p50/p90/p95/p99, CV,
and the evaluated policy in JSON. Failed trials and enforced gates still leave
an artifact.

The seed's bare disk control supports a reproducible local-path measurement,
but is intentionally not the same-HTTPS-front-end L4 control:

```sh
scripts/vex/bench-clone.sh \
  --origin "$GIT_VEX_ORIGIN" \
  --baseline t/vex/out/git-git-disk.git \
  --branch "$GIT_VEX_DEFAULT_BRANCH" \
  --fixture git-git \
  --seed-manifest t/vex/out/seed-manifest.json \
  --runs 5 --warmups 1 \
  --output roadmap/evidence/perf-git-git-local-control.json
```

The L4 disk origin is deliberately not configurable to an arbitrary repository:
for `--origin https://HOST:PORT/git/git.git`, it must be exactly
`https://HOST:PORT/__vex_bench_disk__/git-git-disk.git`. The runner rejects a
different host/port/path and any credentials, query string, or fragment before
it contacts either endpoint. Use that `--disk-origin` plus
`--require-disk-gate`; the runner then requires p50 Vex clone time ≤1.5× disk
p50 and at least five trials with CV≤15%. Add
`BENCH_INCLUDE_GITHUB=1 --require-github` for the exact-ref GitHub gate, and
`--fetch-after-push --require-fetch-after-push-gate` for the ≤2× disk fetch
gate. Fetch measurement briefly moves the chosen branch and restores it with
`--force-with-lease` by default. A passing artifact always keeps
`l4_success_claim:false`; it is evidence for, not completion of, L4.

## FS smart-HTTP integration gate

`scripts/vex/fs-http.py` is a loopback-only test gateway for a bare repository
whose refs and object bytes are actually backed by the Vex FS store.  It
refuses repositories without both `extensions.refstorage=vex://<uuid>` and a
matching `vex.storeRoot`; it is never the Docker classic-bare bootstrap.

Run the read-path proof (an ordinary Git smart-HTTP client process clones and
fetches from the Vex-enabled Git server):

```sh
make -j4 git git-http-backend
scripts/vex/fs-http-smoke.sh --read-only
make -C t T=t1491-vex-fs-http.sh
```

Run without `--read-only` to exercise stock-client push as well.  It is an
honest write-path gate: a receive-pack failure, a missing Vex ref, or classic
object/ref bytes appearing under the thin gitdir causes a failure rather than
being reported as Vex protocol evidence.
