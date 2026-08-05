"""Focused regression check for Vex's smart-HTTP Git-Protocol forwarding."""

import importlib.util
import sys


def load_gateway(path: str):
    spec = importlib.util.spec_from_file_location("vex_mysql_s3_http", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load gateway module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class Headers:
    def __init__(self, *values: str):
        self.values = values

    def get_all(self, name: str, default=None):
        assert name == "Git-Protocol"
        return list(self.values)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test-git-protocol-http.py GATEWAY")
    gateway = load_gateway(sys.argv[1])

    def cgi_env(*values: str) -> dict[str, str]:
        env = {"HTTP_GIT_PROTOCOL": "stale", "OTHER": "preserved"}
        gateway.set_cgi_git_protocol(env, Headers(*values))
        return env

    assert cgi_env("version=2") == {
        "HTTP_GIT_PROTOCOL": "version=2",
        "OTHER": "preserved",
    }
    assert "HTTP_GIT_PROTOCOL" not in cgi_env()
    assert "HTTP_GIT_PROTOCOL" not in cgi_env("version=2", "version=1")
    assert "HTTP_GIT_PROTOCOL" not in cgi_env("version=2\r\nInjected: value")
    assert "HTTP_GIT_PROTOCOL" not in cgi_env("x" * 1025)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
