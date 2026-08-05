# Vex local storage topology

`compose.yaml` starts MySQL 8.4 and MinIO as the intended Vex metadata and
blob-store dependencies.  All host ports explicitly bind to `127.0.0.1`.

Run:

```sh
scripts/vex/up.sh
scripts/vex/seed-small.sh
t/vex/integration-mysql-minio.sh
t/vex/compat-stock-client.sh
scripts/vex/down.sh
```

The supplied `git-http` image is intentionally a **bootstrap**, not the Vex
server: it serves classic bare repositories from `vex_git_bootstrap_repos` and
reports `"vex_storage_active": false` from `/healthz`.  Its successful stock
Git clone/fetch/push checks demonstrate only TLS/smart-HTTP plumbing.  They do
not demonstrate that object bytes use MinIO or that refs use MySQL.

The bootstrap listener itself stays up, but delegates each request to stock
`git http-backend`; it is not the required Vex long-lived data-plane daemon and
must never be used for throughput or clone-SLO claims.

To activate a built Vex-aware, long-lived server, publish it in a derived image
and set `VEX_GIT_HTTP_COMMAND` in `vex/docker/.env`.  That server must consume
the supplied `GIT_VEX_*` variables and expose an honest `/healthz` endpoint.
If it needs a seed hook, also set `VEX_REPO_INIT_COMMAND`; no silent fallback
to a classic bare repository is made once a custom command is configured.
