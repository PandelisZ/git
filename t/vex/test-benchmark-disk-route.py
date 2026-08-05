"""Focused isolation checks for the Vex benchmark-only classic disk route."""

import importlib.util
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def load_gateway(path: str):
    spec = importlib.util.spec_from_file_location("vex_mysql_s3_http", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load gateway module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ConfigReader:
    def __init__(self, gateway):
        self.gateway = gateway
        self.marker = "true"
        self.refstorage = None

    def __call__(self, command, **_kwargs):
        assert command[0] == self.gateway.GIT, command
        key = command[-1]
        if key == self.gateway.BENCH_DISK_MARKER:
            value = self.marker
        elif key == "extensions.refstorage":
            value = self.refstorage
        else:
            raise AssertionError(command)
        if value is None:
            return subprocess.CompletedProcess(command, 1, stdout="", stderr="missing")
        return subprocess.CompletedProcess(command, 0, stdout=value + "\n", stderr="")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test-benchmark-disk-route.py GATEWAY")
    gateway = load_gateway(sys.argv[1])

    previous_root = gateway.BENCH_DISK_ROOT
    previous_run = gateway.subprocess.run
    previous_resolver = gateway.repository_for_path
    previous_reference_backend = os.environ.get("GIT_REFERENCE_BACKEND")
    try:
        with tempfile.TemporaryDirectory(prefix="vex-benchmark-disk-route-") as directory:
            root = Path(directory)
            disk = root / gateway.BENCH_DISK_REPOSITORY
            disk.mkdir()
            (disk / "HEAD").write_text("ref: refs/heads/main\n", encoding="utf-8")
            (disk / "objects").mkdir()

            reader = ConfigReader(gateway)
            gateway.BENCH_DISK_ROOT = root
            gateway.subprocess.run = reader
            os.environ["GIT_REFERENCE_BACKEND"] = "vex://stale"

            repository, is_benchmark_namespace = gateway.repository_for_request(
                gateway.BENCH_DISK_ROUTE + "/info/refs",
                "GET",
                "service=git-upload-pack",
            )
            assert is_benchmark_namespace is True
            assert isinstance(repository, gateway.BenchmarkDiskRepository)
            assert repository.path == disk.resolve()
            assert repository.path_info == "/git-git-disk.git/info/refs"

            disk_env = gateway.cgi_environment_for_repository(
                repository,
                gateway.BENCH_DISK_ROUTE + "/info/refs",
                "service=git-upload-pack",
                "GET",
                "",
                0,
                "127.0.0.1",
            )
            assert disk_env["GIT_PROJECT_ROOT"] == str(root)
            assert disk_env["PATH_INFO"] == "/git-git-disk.git/info/refs"
            assert "GIT_REFERENCE_BACKEND" not in disk_env

            vex_repository = gateway.VexRepository(
                name="fixture.git",
                path=Path("/srv/git/fixture.git"),
                uri="vex://00000000000000000000000000000000",
                repo_id="00000000000000000000000000000000",
            )
            vex_env = gateway.cgi_environment_for_repository(
                vex_repository,
                "/fixture.git/info/refs",
                "service=git-upload-pack",
                "GET",
                "",
                0,
                "127.0.0.1",
            )
            assert vex_env["GIT_PROJECT_ROOT"] == str(gateway.ROOT)
            assert vex_env["PATH_INFO"] == "/fixture.git/info/refs"
            assert vex_env["GIT_REFERENCE_BACKEND"] == vex_repository.uri

            # Reserved-but-invalid paths must stop at the benchmark boundary;
            # they must never become a normal Vex repository lookup.
            gateway.repository_for_path = lambda _path: (_ for _ in ()).throw(
                AssertionError("reserved benchmark path fell through to Vex routing")
            )
            for path, method, query in (
                (gateway.BENCH_DISK_ROUTE + "/objects/aa/" + "0" * 38, "GET", ""),
                (gateway.BENCH_DISK_ROUTE + "/info/refs", "GET", "service=git-upload-pack&x=1"),
                (gateway.BENCH_DISK_NAMESPACE + "/another.git/info/refs", "GET", "service=git-upload-pack"),
            ):
                routed, reserved = gateway.repository_for_request(path, method, query)
                assert reserved is True, path
                assert routed is None, path

            reader.marker = "false"
            routed, reserved = gateway.repository_for_request(
                gateway.BENCH_DISK_ROUTE + "/git-upload-pack", "POST", ""
            )
            assert reserved is True
            assert routed is None

            reader.marker = "true"
            reader.refstorage = "vex://00000000000000000000000000000000"
            routed, reserved = gateway.repository_for_request(
                gateway.BENCH_DISK_ROUTE + "/git-upload-pack", "POST", ""
            )
            assert reserved is True
            assert routed is None
    finally:
        gateway.BENCH_DISK_ROOT = previous_root
        gateway.subprocess.run = previous_run
        gateway.repository_for_path = previous_resolver
        if previous_reference_backend is None:
            os.environ.pop("GIT_REFERENCE_BACKEND", None)
        else:
            os.environ["GIT_REFERENCE_BACKEND"] = previous_reference_backend
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
