# Vex Local Docker Compose Test Environment PRD

**Status:** Active
**Current Stage:** FS metadata + live S3 byte-store slice; MySQL data plane pending
**Owner:** Codex
**Created:** 2026-08-05
**Updated:** 2026-08-05
**Related:** [001](001-cloud-scale-git-forge-storage.md), [004](004-stage-playbooks.md), [005](005-stock-client-verification-and-benchmark.md)

---

## Overview

### Problem

Vex backends are only believable if every developer and CI runner can stand up **the same** MySQL + S3 (+ optional Valkey) stack locally, run stock-git verification, and reproduce benchmarks without AWS accounts or shared staging.

### Implemented boundary (2026-08-05)

`vex/docker/compose.yaml`, `scripts/vex/`, and the local checks are now
present. MySQL schema initialization, MinIO bucket initialization, TLS, and a
stock-Git clone/fetch/push bootstrap were run successfully on loopback.

The bundled `git-http` process deliberately reports
`mode:"bootstrap-classic-bare"` and `vex_storage_active:false`. It is a
transport/dependency proof only: MySQL and MinIO are not yet Vex Git storage
in that process, so no Stage 3/L1 or Stage 4/L2 claim follows from it.

The native `meta-mysql` probe and signed `blob-s3` probe also run
against these Compose services and are green. They use real MySQL and MinIO
protocols and keep raw Git payloads out of MySQL. The central `vex_store` can
now select MinIO canonical bytes with `GIT_VEX_BLOB_BACKEND=s3`, while FS
remains the metadata store for that vertical slice. The bundled `git-http`
process is still not MySQL/S3-backed.

### Goals

1. **All tests and verification are local-first** via Docker Compose.
2. Compose provides **MySQL** (meta SoT) + **MinIO** (S3-compatible blob SoT) + optional **Valkey** + **git smart HTTPS** front.
3. One command path that is **shareable** (committed compose + scripts) and **benchmarkable** (stable ports, seeded git/git mirror, JSON results).
4. No dependency on real AWS/RDS for merge gates.
5. Same code paths as “production” backends (`meta-mysql`, `blob-s3`) — not a separate toy API.

### Non-goals

- Multi-node HA compose (L5).
- **Tenancy or auth product** (IdP, ACL, per-user repos) — one shared basic-auth secret for push is enough for the PoC.
- Replacing unit tests that use the **fs** backend (those stay Docker-free for speed).
- Guaranteeing \(T_{us} \le 1.5 T_{gh}\) inside CI runners with tiny CPUs (CI runs functional + **disk control** SLO; full GitHub comparison is local/nightly bench host).

---

## Design principles

| Principle | Detail |
|-----------|--------|
| Local by default | `docker compose up` is the documented path for L2+ |
| Real drivers | App talks MySQL protocol + S3 API (MinIO), not mocks, for integration tests |
| Deterministic | Fixed project name, ports, credentials in `.env.example` |
| Ephemeral OK | `docker compose down -v` wipes state; seeds re-import |
| Client is stock git | Tests only invoke `git` against `https://localhost:${GIT_HTTPS_PORT}/...` |
| Shareable | Everything under repo: `vex/docker/`, `scripts/vex/`, `t/vex/` |
| Layered | fs unit tests (no Docker) → compose integration → compose bench |

---

## Compose topology

```text
                     stock git client (host)
                              |
                     https://localhost:8443
                              |
                     [caddy/nginx or git-http-backend]
                              |
                     git upload-pack / receive-pack
                     (cloud ODB + cloud refs)
                     /        |         \
                    v         v          v
               mysql:3306  minio:9000  valkey:6379
               (meta SoT)  (blob SoT)  (optional cache)
```

### Services (canonical)

| Service | Image (pin by digest/tag in compose) | Host port (default) | Role |
|---------|--------------------------------------|---------------------|------|
| `mysql` | `mysql:8.4` | `3306` | metadata SoT |
| `minio` | `minio/minio` | `9000` (API), `9001` (console) | S3 blob SoT |
| `minio-init` | `minio/mc` (oneshot) | — | create bucket + policy |
| `valkey` | `valkey/valkey:8` | `6379` | optional tip cache |
| `git-http` | local build / sidecar | `8443` (HTTPS), `8080` (HTTP dev) | **long-lived** smart HTTP front (not CGI-per-request) |
| `seed` (profile `bench`) | oneshot | — | import git/git mirror |

**Profiles:**

- `default` — mysql + minio (+ init) + git-http
- `cache` — adds valkey
- `bench` — adds seed job + volume for git/git mirror cache

### Volumes

| Volume | Purpose |
|--------|---------|
| `vex_mysql_data` | durable meta across restarts (dev) |
| `vex_minio_data` | durable blobs |
| `vex_git_scratch` | pack build scratch on server container |
| `vex_git_mirror` | cached `git clone --mirror https://github.com/git/git` for reseeds |

### Networks

Single bridge `vex_net`. Git server resolves `mysql`, `minio`, `valkey` by compose DNS.

---

## Repository layout (to implement)

```text
vex/docker/
  compose.yaml                 # canonical compose file
  .env.example                 # ports, passwords, bucket, repo uuid
  mysql/
    init/001-schema.sql        # from vex/schema.sql
    conf.d/*.cnf               # optional perf knobs for bench
  minio/
    create-bucket.sh
  caddy/Caddyfile              # or nginx.conf — TLS local cert
  git-http/
    Dockerfile                 # wraps built git + http backend + cloud config
    entrypoint.sh
  certs/                       # mkcert/dev certs or script to generate
scripts/vex/
  up.sh                        # compose up --wait + health
  down.sh                      # compose down
  reset.sh                     # down -v && up && seed-small
  seed-small.sh                # tiny repo for functional tests
  seed-empty-repo.sh           # empty cloud bare for push-stress
  seed-git-git.sh              # mirror import github.com/git/git
  ensure-git-git-mirror.sh     # local --mirror cache of git/git
  smoke-stock-git.sh           # F1 quick path
  bench-clone.sh               # 005 perf harness wrapper
  push-stress.sh               # 009 incremental push stress entry
  print-env.sh                 # exports GIT_VEX_* for host-run gitd
t/vex/
  README.md                    # how to run
  compat-stock-client.sh
  perf-clone-git-git.sh
  integration-mysql-minio.sh
  push-stress/                 # plan/run/profiles/metrics (009)
```

**Compose file path convention:** `vex/docker/compose.yaml`
Invoke: `docker compose -f vex/docker/compose.yaml --env-file vex/docker/.env up -d`

---

## Configuration contract

### `.env.example` (committed)

```env
COMPOSE_PROJECT_NAME=vex
MYSQL_ROOT_PASSWORD=vex-local-root
MYSQL_DATABASE=vex
MYSQL_USER=vex
MYSQL_PASSWORD=vex-local-mysql
MYSQL_PORT=3306

MINIO_ROOT_USER=minioadmin
MINIO_ROOT_PASSWORD=minioadmin
MINIO_API_PORT=9000
MINIO_CONSOLE_PORT=9001
MINIO_BUCKET=git-vex-objects
MINIO_REGION=us-east-1

VALKEY_PORT=6379

GIT_HTTP_PORT=8080
GIT_HTTPS_PORT=8443
GIT_VEX_REPO_GIT_GIT_UUID=00000000-0000-4000-8000-000000000001

# Host-side git client origin
GIT_VEX_ORIGIN=https://localhost:8443/git/git.git
```

### Git server environment (container)

```env
GIT_VEX_BACKEND=mysql+s3
GIT_VEX_BLOB_BACKEND=s3       # implemented: FS metadata + S3 canonical bytes
GIT_VEX_MYSQL_DSN=mysql://vex:vex-local-mysql@mysql:3306/vex
GIT_VEX_S3_ENDPOINT=http://minio:9000
GIT_VEX_S3_BUCKET=git-objects
GIT_VEX_S3_PATH_STYLE=1
GIT_VEX_S3_ACCESS_KEY=...
GIT_VEX_S3_SECRET_KEY=...
GIT_VEX_VALKEY_URL=redis://valkey:6379/0   # optional
GIT_VEX_SCRATCH=/var/git-scratch
```

`GIT_VEX_BACKEND=mysql+s3` names the intended full topology; it does not
activate it yet. The current activation switch is
`GIT_VEX_BLOB_BACKEND=s3`, which preserves FS metadata and directs canonical
object bytes to MinIO. Host-run binary (optional): use the same S3 vars with
`127.0.0.1` ports from published mappings.

### TLS

- Dev: compose generates or mounts a local CA + cert for `localhost` (script `vex/docker/certs/gen.sh` using `openssl` or `mkcert`).
- Stock git: `git -c http.sslCAInfo=vex/docker/certs/rootCA.pem clone https://localhost:8443/...`
  **or** documented one-time `sslCAInfo` in test scripts (still stock git — not a custom client).
- HTTP-only profile `GIT_ALLOW_HTTP=1` on port 8080 for simpler smoke (CI default OK if loopback).

### Push auth (PoC stub only — not a product)

- Compose may provide **one shared HTTP basic auth** password for receive-pack so stock `git push` works in tests.
- **Out of scope:** real users, SSO, tokens, per-repo ACLs, tenancy.
- Credentials only in `vex/docker/.env` (gitignored); `.env.example` has dummy values.
- Client never receives MySQL/MinIO secrets.

### Image pinning & manifests

- Pin MySQL/MinIO/Valkey image **digests** in compose at implement time.
- `seed-git-git.sh` writes `t/vex/out/seed-manifest.json`: HEAD, object count, pack sha256, image digests.
- CI job fails if published ports bind on `0.0.0.0` (must be `127.0.0.1`).

---

## Healthchecks & readiness

| Service | Healthcheck |
|---------|-------------|
| mysql | `mysqladmin ping` |
| minio | `mc ready` / curl health |
| valkey | `valkey-cli ping` |
| git-http | `curl -fk https://localhost:8443/healthz` or `info/refs` probe |

`scripts/vex/up.sh` uses `docker compose up -d --wait` and fails if not healthy in T seconds.

---

## Test layers (local)

### Layer 0 — unit (no Docker)

- fs `vex_store` tests
- pure schema/logic

### Layer 1 — compose integration (default merge gate for L2+)

```sh
scripts/vex/up.sh
scripts/vex/seed-small.sh
t/vex/integration-mysql-minio.sh
t/vex/compat-stock-client.sh   # F1–F8 against small or git/git
scripts/vex/seed-empty-repo.sh stress/git-replay
scripts/vex/push-stress.sh --profile t/vex/push-stress/profiles/smoke.env
scripts/vex/down.sh            # CI always cleanup
```

### Layer 2 — compose functional on git/git (nightly / pre-L4)

```sh
scripts/vex/up.sh --profile bench
scripts/vex/seed-git-git.sh    # uses cached mirror volume if present
t/vex/compat-stock-client.sh ORIGIN=https://localhost:8443/git/git.git
# write-path stress (empty repo replay; not the same as mirror import)
scripts/vex/seed-empty-repo.sh stress/git-replay
scripts/vex/push-stress.sh --profile t/vex/push-stress/profiles/ramp.env
```

### Layer 3 — compose benchmark (local bench host + optional nightly)

```sh
scripts/vex/bench-clone.sh
# writes roadmap/evidence/perf-<date>.json (or t/vex/out/)
```

**CI policy:**

| Check | CI | Laptop/bench |
|-------|----|--------------|
| unit fs | yes | yes |
| compose up + small seed + F1–F3 | yes | yes |
| F1–F8 full | yes if time budget | yes |
| git/git import | nightly | yes |
| \(T_{us}\le 1.5 T_{disk}\) | nightly | yes |
| \(T_{us}\le 1.5 T_{gh}\) | optional nightly (needs network) | **required on release bench host** |

---

## Seeding

### Small seed (`seed-small.sh`)

Creates repo `smoke.git` with a few commits via stock git push to receive-pack — exercises write path without 80k commits.

### git/git seed (`seed-git-git.sh`)

1. If mirror volume empty: `git clone --mirror https://github.com/git/git /mirror/git.git`
2. Import into cloud via one of:
   - `git --git-dir=/mirror/git.git push --mirror https://git-http/git/git.git`
   - or server-side importer reading packs into `vex_store` (faster; still verified by stock `ls-remote`/`clone`)
3. Build L4 precomputed pack/bundle into MinIO (Stage 7).
4. Record `HEAD`, commit count, object count under `t/vex/out/seed-manifest.json`.

---

## Benchmark integration with Compose

`scripts/vex/bench-clone.sh` must:

1. Ensure compose **bench** profile up and seeded.
2. Start/ensure **disk control** bare repo served on same HTTPS front at `/disk/git.git` (classic objects on volume) for \(T_{disk}\).
3. Run N cold-client clones to:
   - `$GIT_VEX_ORIGIN` → \(T_{us}\)
   - disk control URL → \(T_{disk}\)
   - optional `https://github.com/git/git` → \(T_{gh}\) if `BENCH_INCLUDE_GITHUB=1`
4. Emit JSON:

```json
{
  "git_version": "...",
  "host": "...",
  "compose_project": "gitcloud",
  "n": 5,
  "T_us_ms": {"p50": 0, "p95": 0, "samples": []},
  "T_disk_ms": {"p50": 0, "p95": 0, "samples": []},
  "T_gh_ms": {"p50": 0, "p95": 0, "samples": [], "skipped": false},
  "pass_disk_gate": true,
  "pass_github_gate": true,
  "bytes_us": 0,
  "head": "..."
}
```

5. Exit non-zero if gates fail when requested.

---

## Developer UX (shareable README snippet)

```sh
# one-time
cp vex/docker/.env.example vex/docker/.env
scripts/vex/up.sh

# functional
scripts/vex/seed-small.sh
git -c http.sslCAInfo=vex/docker/certs/rootCA.pem \
  clone https://localhost:8443/smoke/smoke.git /tmp/smoke

# full verification
scripts/vex/seed-git-git.sh
t/vex/compat-stock-client.sh
scripts/vex/bench-clone.sh

# reset world
scripts/vex/reset.sh
```

Anyone with Docker + git can reproduce.

---

## Security notes (dev / experiment only)

- **PoC stack** — not a multi-tenant production forge; **tenancy/auth products out of scope**.
- Default passwords are public and **dev-only**; one shared push password is enough for tests.
- Compose must not be used as production deploy.
- MinIO credentials only on compose network + published ports bound to `127.0.0.1` by default.

```yaml
ports:
  - "127.0.0.1:3306:3306"
  - "127.0.0.1:9000:9000"
  - "127.0.0.1:8443:8443"
```

### Resource limits (stress profiles — W5-10)

Demand cgroup limits so push-stress cannot silently eat the host:

| Service | cpus | memory | notes |
|---------|------|--------|-------|
| git-http | 4 | 8G | pids/nofile raised |
| mysql | 2 | 4G | max-connections capped |
| minio | 2 | 4G | |
| pack/sweeper jobs | 2 | 4G | |

Healthchecks + `restart: on-failure`. Exact YAML at implement time.

---

## Stage mapping

| Stage | Compose need |
|-------|----------------|
| 1 fs unit | none |
| 2–3 protocol fs | optional |
| **4 MySQL+S3** | **compose required** — first consumer of `compose.yaml` |
| 5 Valkey | profile `cache` |
| 6–6b sweeper/scrub | compose + jobs container or host cron scripts |
| **7 L4 acceptance** | **compose profile `bench` + seed-git-git + bench-clone** |
| 8 HA | out of scope for this compose file |

---

## Acceptance criteria (for this PRD when implemented)

- [ ] `vex/docker/compose.yaml` committed and documented
- [ ] `scripts/vex/up.sh` healthy in ≤ 2 minutes on laptop
- [ ] Stock `git clone` to localhost works against mysql+minio backend
- [ ] `down -v && up && seed-small` is idempotent
- [ ] CI job runs compose integration without cloud credentials
- [ ] Bench script produces JSON artifact and disk-gate evaluation
- [ ] Ports bound to localhost by default

---

## Evidence Log

### 2026-08-05

- User requirement: tests/verification local; local S3 + MySQL via Docker Compose; controllable, testable, shareable, benchmarkable.
- Aligns with Stage 4+ backends and 005 stock-client gates.
- MinIO as S3 stand-in; MySQL 8; optional Valkey; smart HTTP on localhost.

## Open Questions

- [ ] Caddy vs nginx vs raw `git-http-backend` + stunnel for TLS
- [ ] Whether git server runs **in** compose or on host against published ports (recommend **in compose** for shareability; host binary optional)
- [ ] Pin exact image digests at implement time
