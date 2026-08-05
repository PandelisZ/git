#!/usr/bin/env python3
"""Loopback smart-HTTP gateway for an FS-backed Vex repository.

This is deliberately a small integration harness, not the production Vex
data-plane service.  It serves only repositories that have an on-disk
``extensions.refStorage=vex://<uuid>`` configuration, passes that UUID and
the configured FS store to every ``git http-backend`` process, and binds to
loopback by default.  That makes a successful client run meaningful Vex-FS
protocol evidence without conflating it with the Docker classic-bare
bootstrap or with the future MySQL/MinIO service.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import tempfile
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Iterable
from urllib.parse import unquote, urlsplit


REPO_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\.git\Z")
VEX_URI_RE = re.compile(r"vex://([0-9a-fA-F]{32})\Z")


def fail(message: str) -> "None":
    raise SystemExit(f"vex fs-http: {message}")


def git_output(git: str, args: list[str]) -> str:
    completed = subprocess.run(
        [git, *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode:
        detail = completed.stderr.strip() or "unknown Git error"
        fail(f"{' '.join(args)} failed: {detail}")
    return completed.stdout.strip()


@dataclass(frozen=True)
class VexRepository:
    name: str
    path: Path
    uri: str
    repo_id: str


class VexRepositoryIndex:
    """Read and validate the explicit Vex repositories exposed by this server."""

    def __init__(self, git: str, root: Path, store: Path) -> None:
        self.git = git
        self.root = root
        self.store = store
        self.repositories = self._load()

    def _config(self, path: Path, key: str) -> str:
        return git_output(self.git, [f"--git-dir={path}", "config", "--get", key])

    def _load(self) -> dict[str, VexRepository]:
        result: dict[str, VexRepository] = {}
        for candidate in sorted(self.root.iterdir()):
            if not candidate.is_dir() or not REPO_NAME_RE.fullmatch(candidate.name):
                continue
            if not (candidate / "HEAD").is_file():
                continue
            uri = self._config(candidate, "extensions.refstorage")
            match = VEX_URI_RE.fullmatch(uri)
            if not match:
                fail(f"{candidate} is not configured for Vex storage")
            configured_store = Path(self._config(candidate, "vex.storeroot")).resolve()
            if configured_store != self.store:
                fail(
                    f"{candidate} uses vex.storeRoot={configured_store}, "
                    f"not requested store {self.store}"
                )
            repo_id = match.group(1).lower()
            result[candidate.name] = VexRepository(
                name=candidate.name,
                path=candidate,
                uri=f"vex://{repo_id}",
                repo_id=repo_id,
            )
        if not result:
            fail(f"no Vex bare repositories found under {self.root}")
        return result

    def lookup(self, path: str) -> VexRepository | None:
        decoded = unquote(path)
        parts = [part for part in decoded.split("/") if part]
        if not parts or ".." in parts or "\x00" in decoded:
            return None
        return self.repositories.get(parts[0])


def split_cgi_response(raw: bytes) -> tuple[int, list[tuple[str, str]], bytes]:
    if b"\r\n\r\n" in raw:
        header_bytes, body = raw.split(b"\r\n\r\n", 1)
        lines = header_bytes.split(b"\r\n")
    elif b"\n\n" in raw:
        header_bytes, body = raw.split(b"\n\n", 1)
        lines = header_bytes.split(b"\n")
    else:
        return 502, [("Content-Type", "text/plain; charset=utf-8")], b"git http-backend returned malformed CGI output\n"

    status = 200
    headers: list[tuple[str, str]] = []
    for line in lines:
        if not line or b":" not in line:
            continue
        name, value = line.split(b":", 1)
        name_text = name.decode("latin-1").strip()
        value_text = value.decode("latin-1").strip()
        if name_text.lower() == "status":
            try:
                status = int(value_text.split(None, 1)[0])
            except (IndexError, ValueError):
                status = 502
            continue
        if name_text.lower() not in {"connection", "transfer-encoding"}:
            headers.append((name_text, value_text))
    return status, headers, body


class VexFSHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(
        self,
        bind: str,
        port: int,
        git: str,
        http_backend: str,
        git_exec_path: str,
        helper_path: str,
        root: Path,
        store: Path,
        max_request_bytes: int,
    ) -> None:
        self.git = git
        self.http_backend = http_backend
        self.git_exec_path = git_exec_path
        self.helper_path = helper_path
        self.root = root
        self.store = store
        self.max_request_bytes = max_request_bytes
        self.repositories = VexRepositoryIndex(git, root, store)
        super().__init__((bind, port), VexFSHTTPRequestHandler)


class VexFSHTTPRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "VexFSHTTP/0"

    @property
    def vex_server(self) -> VexFSHTTPServer:
        return self.server  # type: ignore[return-value]

    def log_message(self, fmt: str, *args: object) -> None:
        sys.stderr.write("vex fs-http: %s\n" % (fmt % args))

    def do_GET(self) -> None:
        self._serve()

    def do_HEAD(self) -> None:
        self._serve()

    def do_POST(self) -> None:
        self._serve()

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Allow", "GET, HEAD, POST, OPTIONS")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _healthz(self) -> None:
        server = self.vex_server
        payload = {
            "mode": "vex-fs-test",
            "repositories": {
                name: repo.repo_id for name, repo in sorted(server.repositories.repositories.items())
            },
            "service": "vex-fs-git-http",
            "storage_backend": "fs",
            "vex_storage_active": True,
            "warning": "loopback integration harness; not MySQL/MinIO or production service evidence",
        }
        body = (json.dumps(payload, sort_keys=True) + "\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _read_chunked_body(self) -> bytes | None:
        chunks: list[bytes] = []
        length = 0
        while True:
            line = self.rfile.readline(8192)
            if not line:
                self.send_error(400, "truncated chunked request")
                return None
            try:
                chunk_length = int(line.split(b";", 1)[0].strip(), 16)
            except ValueError:
                self.send_error(400, "invalid chunked request")
                return None
            if chunk_length == 0:
                while True:
                    trailer = self.rfile.readline(8192)
                    if trailer in {b"\r\n", b"\n", b""}:
                        return b"".join(chunks)
            length += chunk_length
            if length > self.vex_server.max_request_bytes:
                self.send_error(413, "request exceeds Vex FS test limit")
                return None
            chunk = self.rfile.read(chunk_length)
            ending = self.rfile.read(2)
            if len(chunk) != chunk_length or ending != b"\r\n":
                self.send_error(400, "truncated chunked request")
                return None
            chunks.append(chunk)

    def _read_body(self) -> bytes | None:
        if self.command != "POST":
            return b""
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            return self._read_chunked_body()
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            self.send_error(411, "Content-Length is required")
            return None
        try:
            length = int(raw_length)
        except ValueError:
            self.send_error(400, "invalid Content-Length")
            return None
        if length < 0 or length > self.vex_server.max_request_bytes:
            self.send_error(413, "request exceeds Vex FS test limit")
            return None
        return self.rfile.read(length)

    def _serve(self) -> None:
        split = urlsplit(self.path)
        if split.path == "/healthz":
            self._healthz()
            return

        repository = self.vex_server.repositories.lookup(split.path)
        if repository is None:
            self.send_error(404, "unknown Vex repository")
            return
        body = self._read_body()
        if body is None:
            return

        environ = dict(os.environ)
        for key in (
            "GIT_DIR",
            "GIT_WORK_TREE",
            "GIT_INDEX_FILE",
            "GIT_OBJECT_DIRECTORY",
            "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        ):
            environ.pop(key, None)
        environ.update(
            {
                "GIT_EXEC_PATH": self.vex_server.git_exec_path,
                "GIT_HTTP_EXPORT_ALL": "1",
                "GIT_PROJECT_ROOT": str(self.vex_server.root),
                "GIT_REFERENCE_BACKEND": repository.uri,
                "GIT_VEX_FS_ROOT": str(self.vex_server.store),
                "PATH_INFO": unquote(split.path),
                "QUERY_STRING": split.query,
                "REQUEST_METHOD": self.command,
                "CONTENT_TYPE": self.headers.get("Content-Type", ""),
                "CONTENT_LENGTH": str(len(body)),
                "REMOTE_ADDR": self.client_address[0],
            }
        )
        environ["PATH"] = self.vex_server.helper_path + os.pathsep + environ.get("PATH", "")
        try:
            completed = subprocess.run(
                [self.vex_server.http_backend],
                input=body,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environ,
                timeout=300,
                check=False,
            )
        except subprocess.TimeoutExpired:
            self.send_error(504, "git http-backend timed out")
            return
        if completed.returncode:
            sys.stderr.write(completed.stderr.decode("utf-8", "replace"))

        status, headers, response_body = split_cgi_response(completed.stdout)
        self.send_response(status)
        saw_length = False
        for name, value in headers:
            if name.lower() == "content-length":
                saw_length = True
            self.send_header(name, value)
        if not saw_length:
            self.send_header("Content-Length", str(len(response_body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(response_body)
        self.close_connection = True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="serve explicit FS-backed Vex repos to normal Git smart-HTTP clients"
    )
    parser.add_argument("--git", required=True, help="Vex-enabled Git executable")
    parser.add_argument(
        "--http-backend",
        help="built git-http-backend from the same Vex source build (default: sibling of --git)",
    )
    parser.add_argument(
        "--git-exec-path",
        help="directory containing the source Git executable and built-ins (default: --git parent)",
    )
    parser.add_argument("--root", required=True, help="directory holding single-level *.git repos")
    parser.add_argument("--store", required=True, help="FS Vex store root shared by those repos")
    parser.add_argument("--bind", default="127.0.0.1", help="listener address (default: loopback)")
    parser.add_argument("--port", type=int, default=0, help="listener port (default: ephemeral)")
    parser.add_argument("--ready-file", help="write the HTTP base URL here once listening")
    parser.add_argument(
        "--max-request-bytes",
        type=int,
        default=64 * 1024 * 1024,
        help="maximum HTTP POST body accepted by this integration harness",
    )
    return parser.parse_args()


def make_helper_path(git: str) -> Path:
    """Make server-side dashed commands resolve to the exact Vex Git binary.

    A source-tree ``git`` is often built with an install-time GIT_EXEC_PATH
    that does not exist yet.  http-backend starts upload-pack/receive-pack by
    dashed command name, so a tiny ephemeral PATH prefix keeps all server
    children on the built binary instead of accidentally selecting host Git.
    """
    helper_path = Path(tempfile.mkdtemp(prefix="vex-fs-http-exec-"))
    for name in ("git", "git-upload-pack", "git-receive-pack"):
        os.symlink(git, helper_path / name)
    return helper_path


def main() -> int:
    args = parse_args()
    if args.port < 0 or args.port > 65535:
        fail("--port must be between 0 and 65535")
    if args.max_request_bytes <= 0:
        fail("--max-request-bytes must be positive")
    git = str(Path(args.git).resolve())
    if not os.path.isfile(git) or not os.access(git, os.X_OK):
        fail(f"--git is not executable: {git}")
    http_backend = str(Path(args.http_backend).resolve()) if args.http_backend else str(Path(git).with_name("git-http-backend"))
    if not os.path.isfile(http_backend) or not os.access(http_backend, os.X_OK):
        fail(
            f"git-http-backend is not executable: {http_backend}; "
            "build it with `make git-http-backend` or pass --http-backend"
        )
    root = Path(args.root).resolve()
    store = Path(args.store).resolve()
    if not root.is_dir():
        fail(f"--root does not exist: {root}")
    if not store.is_dir():
        fail(f"--store does not exist: {store}")
    git_exec_path = str(Path(args.git_exec_path).resolve()) if args.git_exec_path else str(Path(git).parent)
    if not Path(git_exec_path).is_dir():
        fail(f"--git-exec-path does not exist: {git_exec_path}")
    helper_path = make_helper_path(git)
    server = VexFSHTTPServer(
        args.bind,
        args.port,
        git,
        http_backend,
        git_exec_path,
        str(helper_path),
        root,
        store,
        args.max_request_bytes,
    )
    url = f"http://{args.bind}:{server.server_port}"
    if args.ready_file:
        ready = Path(args.ready_file)
        ready.parent.mkdir(parents=True, exist_ok=True)
        temporary = ready.with_name(ready.name + ".tmp")
        temporary.write_text(url + "\n", encoding="utf-8")
        temporary.replace(ready)
    print(f"vex fs-http: listening on {url} for {len(server.repositories.repositories)} Vex-FS repository(s)", flush=True)

    stopped = threading.Event()

    def stop(_signum: int, _frame: object) -> None:
        if not stopped.is_set():
            stopped.set()
            # BaseServer.shutdown() waits for serve_forever() to finish, so it
            # must not run on the serve_forever signal-handling thread.
            threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        server.serve_forever()
    finally:
        server.server_close()
        shutil.rmtree(helper_path, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
