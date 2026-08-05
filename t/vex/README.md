# Vex test harnesses

`push-stress/` exercises an empty target repository through unmodified Git
transport commands. It is usable against a classic bare repository today and
becomes the write-path gate for the Vex backend once its smart-HTTP endpoint is
available.

Run the self-contained local proof:

```sh
t/vex/push-stress/test-plan-determinism.sh
t/vex/push-stress/test-classic-bare.sh
```

See [push-stress/README.md](push-stress/README.md) for profiles, artifacts, and
the server-owned canonical-byte reconciliation hook.

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
