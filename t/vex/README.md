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
