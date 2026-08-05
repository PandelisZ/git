"""Focused bounded-health-inventory checks for the Vex smart-HTTP gateway."""

import importlib.util
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


def write_repository(root: Path, name: str, uri: str, config: str | None = None) -> Path:
    repository = root.joinpath(*name.split("/"))
    repository.mkdir(parents=True)
    (repository / "HEAD").write_text("ref: refs/heads/main\n", encoding="utf-8")
    (repository / "config").write_text(
        config
        if config is not None
        else "[core]\n\trepositoryformatversion = 0\n[extensions]\n\trefStorage = " + uri + "\n",
        encoding="utf-8",
    )
    return repository


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test-health-repository-discovery.py GATEWAY")
    gateway = load_gateway(sys.argv[1])

    previous_root = gateway.ROOT
    previous_run = gateway.subprocess.run
    previous_limits = (
        gateway.HEALTH_SCAN_MAX_REPOSITORIES,
        gateway.HEALTH_SCAN_MAX_DIRECTORIES,
        gateway.HEALTH_SCAN_MAX_DIRECTORY_ENTRIES,
        gateway.HEALTH_SCAN_MAX_DEPTH,
        gateway.HEALTH_SCAN_MAX_CONFIG_BYTES,
    )
    try:
        with tempfile.TemporaryDirectory(prefix="vex-health-repository-discovery-") as directory:
            root = Path(directory).resolve()
            direct_uri = "vex://11111111111111111111111111111111"
            nested_uri = "vex://22222222222222222222222222222222"
            hidden_uri = "vex://33333333333333333333333333333333"
            direct = write_repository(root, "healthy.git", direct_uri)
            write_repository(root, "team/active.git", nested_uri)
            stalled = write_repository(root, "git/git.git", hidden_uri)
            # A recursive rglob-based scanner would enter this candidate bare
            # repository and discover the nested fake repository.  Health
            # discovery must stop at the git/git scaffold instead.
            write_repository(stalled / "objects", "unreachable.git", hidden_uri)
            # Health also must not follow a repository-shaped symlink.
            (root / "linked.git").symlink_to(stalled, target_is_directory=True)

            gateway.ROOT = root
            gateway.HEALTH_SCAN_MAX_REPOSITORIES = 16
            gateway.HEALTH_SCAN_MAX_DIRECTORIES = 16
            gateway.HEALTH_SCAN_MAX_DIRECTORY_ENTRIES = 16
            gateway.HEALTH_SCAN_MAX_DEPTH = 8

            calls: list[list[str]] = []

            def forbidden_git(command, **_kwargs):
                calls.append(command)
                raise AssertionError("health inventory invoked custom Git")

            gateway.subprocess.run = forbidden_git
            assert gateway.known_repositories() == {
                "healthy.git": direct_uri.removeprefix("vex://"),
                "team/active.git": nested_uri.removeprefix("vex://"),
                "git/git.git": hidden_uri.removeprefix("vex://"),
            }
            assert calls == []

            # The compact parser intentionally fails closed for config forms
            # which need Git's include/quoting semantics or exceed its cap.
            ambiguous = write_repository(
                root,
                "ambiguous.git",
                direct_uri,
                "[include]\n\tpath = /outside/config\n[extensions]\n\trefStorage = " + direct_uri + "\n",
            )
            duplicate = write_repository(
                root,
                "duplicate.git",
                direct_uri,
                "[extensions]\n\trefStorage = "
                + direct_uri
                + "\n\trefStorage = "
                + nested_uri
                + "\n",
            )
            quoted = write_repository(
                root,
                "quoted.git",
                direct_uri,
                "[extensions]\n\trefStorage = \"" + direct_uri + "\"\n",
            )
            assert gateway.health_repository_uri(ambiguous) is None
            assert gateway.health_repository_uri(duplicate) is None
            assert gateway.health_repository_uri(quoted) is None

            gateway.HEALTH_SCAN_MAX_CONFIG_BYTES = 8
            assert gateway.health_repository_uri(direct) is None
            gateway.HEALTH_SCAN_MAX_CONFIG_BYTES = previous_limits[4]

            # A directory cap stops recursion before a nested repository can
            # turn the health response into an unbounded filesystem walk.
            gateway.HEALTH_SCAN_MAX_DIRECTORIES = 1
            assert gateway.known_repositories() == {
                "healthy.git": direct_uri.removeprefix("vex://"),
            }

            # Normal request routing retains the authoritative Git config
            # validation; health's filesystem-only fast path never replaces
            # it.
            def routing_git(command, **_kwargs):
                calls.append(command)
                return subprocess.CompletedProcess(command, 0, stdout=direct_uri + "\n", stderr="")

            gateway.subprocess.run = routing_git
            routed = gateway.repository_for_path("/healthy.git/info/refs")
            assert routed is not None
            assert routed.name == "healthy.git"
            assert routed.uri == direct_uri
            assert len(calls) == 1
    finally:
        gateway.ROOT = previous_root
        gateway.subprocess.run = previous_run
        (
            gateway.HEALTH_SCAN_MAX_REPOSITORIES,
            gateway.HEALTH_SCAN_MAX_DIRECTORIES,
            gateway.HEALTH_SCAN_MAX_DIRECTORY_ENTRIES,
            gateway.HEALTH_SCAN_MAX_DEPTH,
            gateway.HEALTH_SCAN_MAX_CONFIG_BYTES,
        ) = previous_limits
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
