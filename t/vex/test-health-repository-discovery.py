"""Focused bounded-health-inventory checks for the Vex smart-HTTP gateway."""

import importlib.util
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
        gateway.REPOSITORY_CONFIG_MAX_BYTES,
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
            assert gateway.vex_repository_uri_from_config(ambiguous) is None
            assert gateway.vex_repository_uri_from_config(duplicate) is None
            assert gateway.vex_repository_uri_from_config(quoted) is None

            gateway.REPOSITORY_CONFIG_MAX_BYTES = 8
            assert gateway.vex_repository_uri_from_config(direct) is None
            gateway.REPOSITORY_CONFIG_MAX_BYTES = previous_limits[4]

            # A directory cap stops recursion before a nested repository can
            # turn the health response into an unbounded filesystem walk.
            gateway.HEALTH_SCAN_MAX_DIRECTORIES = 1
            assert gateway.known_repositories() == {
                "healthy.git": direct_uri.removeprefix("vex://"),
            }

            # Smart-HTTP routing uses the same bounded local parser. It must
            # not initialize custom Git merely to discover refstorage: that
            # can hydrate a large Vex pack cache before the CGI starts.
            routed = gateway.repository_for_path("/healthy.git/info/refs")
            assert routed is not None
            assert routed.name == "healthy.git"
            assert routed.uri == direct_uri
            assert calls == []
            assert gateway.repository_for_path("/ambiguous.git/info/refs") is None
    finally:
        gateway.ROOT = previous_root
        gateway.subprocess.run = previous_run
        (
            gateway.HEALTH_SCAN_MAX_REPOSITORIES,
            gateway.HEALTH_SCAN_MAX_DIRECTORIES,
            gateway.HEALTH_SCAN_MAX_DIRECTORY_ENTRIES,
            gateway.HEALTH_SCAN_MAX_DEPTH,
            gateway.REPOSITORY_CONFIG_MAX_BYTES,
        ) = previous_limits
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
