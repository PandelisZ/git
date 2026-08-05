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
