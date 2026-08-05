#!/usr/bin/env python3
"""Long-lived smart-HTTP gateway for the Vex MySQL + S3 data plane.

The listener itself is deliberately small: it validates the thin Vex
repository named by each request and then delegates the Git protocol to the
Vex-enabled ``git-http-backend`` built into this image.  Metadata and refs are
therefore opened through the MySQL Vex store, while canonical object bytes are
opened through the S3-compatible blob store.  It never falls back to a
classic bare repository when a path is not Vex-configured.
"""

from __future__ import annotations

import base64
import hmac
import json
import os
import re
import signal
import ssl
import subprocess
import sys
import threading
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Iterable
from urllib.parse import unquote, urlsplit


ROOT = Path(os.environ.get("GIT_HTTP_ROOT", "/srv/git")).resolve()
BIND = os.environ.get("VEX_HTTP_BIND", "0.0.0.0")
HTTP_PORT = int(os.environ.get("VEX_HTTP_PORT", "8080"))
HTTPS_PORT = int(os.environ.get("VEX_HTTPS_PORT", "8443"))
ALLOW_HTTP = os.environ.get("GIT_ALLOW_HTTP", "1") == "1"
REQUIRE_PUSH_AUTH = os.environ.get("VEX_HTTP_REQUIRE_PUSH_AUTH", "1") == "1"
BASIC_USER = os.environ.get("VEX_HTTP_BASIC_USER", "vex")
BASIC_PASSWORD = os.environ.get("VEX_HTTP_BASIC_PASSWORD", "")
MAX_REQUEST_BYTES = int(os.environ.get("VEX_HTTP_MAX_REQUEST_BYTES", "67108864"))
CERT = os.environ.get("VEX_HTTP_TLS_CERT", "/certs/localhost.pem")
KEY = os.environ.get("VEX_HTTP_TLS_KEY", "/certs/localhost.key")
GIT = os.environ.get("VEX_GIT", "/opt/vex-git/git")
HTTP_BACKEND = os.environ.get("VEX_GIT_HTTP_BACKEND", "/opt/vex-git/git-http-backend")
GIT_EXEC_PATH = os.environ.get("VEX_GIT_EXEC_PATH", "/opt/vex-git")
HELPER_PATH = os.environ.get("VEX_GIT_HELPER_PATH", "/usr/local/lib/vex-git-http/bin")
INIT_REPOSITORY = os.environ.get("VEX_NATIVE_REPO_INIT", "/usr/local/bin/vex-init-mysql-s3-repo")
READY_REPOSITORY = os.environ.get("VEX_STORAGE_READY_REPO", "vex-health.git")

REPO_COMPONENT_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\Z")
VEX_URI_RE = re.compile(r"vex://([0-9a-fA-F]{32})\Z")
GIT_PROTOCOL_HEADER = "Git-Protocol"
# This is far larger than the normal ``version=2`` request while preventing a
# client-controlled HTTP field from becoming an unbounded CGI environment
# value.  The protocol specification carries its extra parameters in this
# one colon-separated field.
MAX_GIT_PROTOCOL_HEADER_BYTES = 1024


def fail(message: str) -> "None":
    raise SystemExit(f"vex mysql-s3-http: {message}")


def sanitized_git_protocol_header(headers: object) -> str | None:
    """Return one safe Git-Protocol request header for CGI forwarding.

    ``git-http-backend`` recognizes protocol-v2 negotiation only after its
    CGI ``HTTP_GIT_PROTOCOL`` variable is populated.  Keep the original
    printable-ASCII value (including colon-separated extra parameters), but
    reject ambiguous repeated headers, controls such as CR/LF, and oversized
    values before putting client input in the child environment.
    """
    get_all = getattr(headers, "get_all", None)
    if get_all is None:
        return None
    values = get_all(GIT_PROTOCOL_HEADER, [])
    if len(values) != 1:
        return None
    value = values[0].strip()
    if not value or len(value) > MAX_GIT_PROTOCOL_HEADER_BYTES:
        return None
    if not all(" " <= char <= "~" for char in value):
        return None
    return value


def set_cgi_git_protocol(environ: dict[str, str], headers: object) -> None:
    """Set the one request-scoped protocol CGI variable, if it is valid."""
    environ.pop("HTTP_GIT_PROTOCOL", None)
    value = sanitized_git_protocol_header(headers)
    if value is not None:
        environ["HTTP_GIT_PROTOCOL"] = value


def split_cgi_response(raw: bytes) -> tuple[int, list[tuple[str, str]], bytes]:
    """Return a validated CGI status, headers, and body."""
    if b"\r\n\r\n" in raw:
        header_bytes, body = raw.split(b"\r\n\r\n", 1)
        lines = header_bytes.split(b"\r\n")
    elif b"\n\n" in raw:
        header_bytes, body = raw.split(b"\n\n", 1)
        lines = header_bytes.split(b"\n")
    else:
        return (
            502,
            [("Content-Type", "text/plain; charset=utf-8")],
            b"git-http-backend returned malformed CGI output\n",
        )

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


@dataclass(frozen=True)
class VexRepository:
    name: str
    path: Path
    uri: str
    repo_id: str


def repository_for_path(path: str) -> VexRepository | None:
    """Resolve one URL to a configured Vex thin repository.

    Smart-HTTP paths always place the repository before ``info/refs`` or a
    service name.  Finding the first ``*.git`` component supports both
    ``smoke.git`` and nested names such as ``stress/git-replay.git`` without
    walking unrelated directories for every request.
    """
    decoded = unquote(path)
    parts = [part for part in decoded.split("/") if part]
    if not parts or ".." in parts or "\x00" in decoded:
        return None

    repo_parts: list[str] = []
    for part in parts:
        if not REPO_COMPONENT_RE.fullmatch(part):
            return None
        repo_parts.append(part)
        if part.endswith(".git"):
            break
    else:
        return None

    try:
        candidate = (ROOT.joinpath(*repo_parts)).resolve()
        candidate.relative_to(ROOT)
    except ValueError:
        return None
    if not candidate.is_dir() or not (candidate / "HEAD").is_file():
        return None

    completed = subprocess.run(
        [GIT, f"--git-dir={candidate}", "config", "--get", "extensions.refstorage"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=30,
    )
    if completed.returncode:
        return None
    match = VEX_URI_RE.fullmatch(completed.stdout.strip())
    if not match:
        return None
    repo_id = match.group(1).lower()
    return VexRepository(
        name="/".join(repo_parts),
        path=candidate,
        uri=f"vex://{repo_id}",
        repo_id=repo_id,
    )


def known_repositories() -> dict[str, str]:
    """Return the small local topology's configured Vex repositories."""
    result: dict[str, str] = {}
    for candidate in sorted(ROOT.rglob("*.git")):
        if not candidate.is_dir() or not (candidate / "HEAD").is_file():
            continue
        relative = candidate.relative_to(ROOT).as_posix()
        repo = repository_for_path("/" + relative + "/info/refs")
        if repo:
            result[repo.name] = repo.repo_id
    return result


class VexMySQLS3HTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def storage_ready(self) -> tuple[bool, str]:
        """Exercise the exact Vex store selected by the serving Git binary.

        ``for-each-ref`` opens the Vex ref store.  Its initialization path
        checks the MySQL schema/repository row and the S3 bucket, so health
        only reports the backend active while both configured dependencies can
        be opened by custom Git.
        """
        repo_path = ROOT / READY_REPOSITORY
        try:
            completed = subprocess.run(
                [GIT, f"--git-dir={repo_path}", "for-each-ref"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
                timeout=30,
            )
        except subprocess.TimeoutExpired:
            return False, "Vex storage readiness command timed out"
        if completed.returncode:
            detail = completed.stderr.strip() or "Vex storage readiness command failed"
            return False, detail
        return True, ""


class VexMySQLS3HTTPRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "VexMySQLS3HTTP/0"

    @property
    def vex_server(self) -> VexMySQLS3HTTPServer:
        return self.server  # type: ignore[return-value]

    def log_message(self, fmt: str, *args: object) -> None:
        sys.stderr.write("vex mysql-s3-http: %s\n" % (fmt % args))

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
        ready, detail = self.vex_server.storage_ready()
        payload = {
            "blob_backend": "s3",
            "metadata_backend": "mysql",
            "mode": "vex-mysql-s3",
            "repositories": known_repositories() if ready else {},
            "service": "vex-mysql-s3-git-http",
            "storage_backend": "mysql+s3",
            "vex_storage_active": ready,
        }
        if ready:
            payload["storage_ready_repository"] = READY_REPOSITORY
        else:
            payload["message"] = detail
        body = (json.dumps(payload, sort_keys=True) + "\n").encode("utf-8")
        self.send_response(200 if ready else 503)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _needs_push_auth(self, parsed_path: str, query: str) -> bool:
        if not REQUIRE_PUSH_AUTH:
            return False
        if parsed_path.endswith("/git-receive-pack"):
            return True
        return parsed_path.endswith("/info/refs") and "service=git-receive-pack" in query

    def _authorized(self) -> bool:
        header = self.headers.get("Authorization", "")
        if not header.startswith("Basic "):
            return False
        try:
            raw = base64.b64decode(header[6:], validate=True).decode("utf-8")
        except (UnicodeDecodeError, ValueError):
            return False
        supplied = raw.encode("utf-8")
        expected = (BASIC_USER + ":" + BASIC_PASSWORD).encode("utf-8")
        return hmac.compare_digest(supplied, expected)

    def _auth_required(self) -> None:
        body = b"authentication required for git-receive-pack\n"
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="Vex local push"')
        self.send_header("Content-Type", "text/plain; charset=utf-8")
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
            if length > MAX_REQUEST_BYTES:
                self.send_error(413, "request exceeds Vex HTTP limit")
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
        if length < 0 or length > MAX_REQUEST_BYTES:
            self.send_error(413, "request exceeds Vex HTTP limit")
            return None
        return self.rfile.read(length)

    def _serve(self) -> None:
        split = urlsplit(self.path)
        if split.path == "/healthz":
            self._healthz()
            return
        if not ALLOW_HTTP and not isinstance(self.connection, ssl.SSLSocket):
            self.send_error(404)
            return

        repository = repository_for_path(split.path)
        if repository is None:
            self.send_error(404, "unknown Vex repository")
            return
        if self._needs_push_auth(split.path, split.query) and not self._authorized():
            self._auth_required()
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
                "GIT_EXEC_PATH": GIT_EXEC_PATH,
                "GIT_HTTP_EXPORT_ALL": "1",
                "GIT_PROJECT_ROOT": str(ROOT),
                "GIT_REFERENCE_BACKEND": repository.uri,
                "PATH_INFO": unquote(split.path),
                "QUERY_STRING": split.query,
                "REQUEST_METHOD": self.command,
                "CONTENT_TYPE": self.headers.get("Content-Type", ""),
                "CONTENT_LENGTH": str(len(body)),
                "REMOTE_ADDR": self.client_address[0],
            }
        )
        environ["PATH"] = HELPER_PATH + os.pathsep + environ.get("PATH", "")
        set_cgi_git_protocol(environ, self.headers)
        if self._authorized():
            environ["REMOTE_USER"] = BASIC_USER
        try:
            completed = subprocess.run(
                [HTTP_BACKEND],
                input=body,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environ,
                timeout=300,
                check=False,
            )
        except subprocess.TimeoutExpired:
            self.send_error(504, "git-http-backend timed out")
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


def require_configuration() -> None:
    required = (
        "GIT_VEX_META_BACKEND",
        "GIT_VEX_BLOB_BACKEND",
        "GIT_VEX_MYSQL_HOST",
        "GIT_VEX_MYSQL_PORT",
        "GIT_VEX_MYSQL_USER",
        "GIT_VEX_MYSQL_PASSWORD",
        "GIT_VEX_MYSQL_DATABASE",
        "GIT_VEX_S3_ENDPOINT",
        "GIT_VEX_S3_BUCKET",
        "GIT_VEX_S3_ACCESS_KEY",
        "GIT_VEX_S3_SECRET_KEY",
    )
    missing = [name for name in required if not os.environ.get(name)]
    if missing:
        fail("missing required Vex backend configuration: " + ", ".join(missing))
    if os.environ["GIT_VEX_META_BACKEND"] != "mysql":
        fail("GIT_VEX_META_BACKEND must be mysql")
    if os.environ["GIT_VEX_BLOB_BACKEND"] != "s3":
        fail("GIT_VEX_BLOB_BACKEND must be s3")
    for executable, label in ((GIT, "Vex Git"), (HTTP_BACKEND, "git-http-backend"), (INIT_REPOSITORY, "repository initializer")):
        if not os.path.isfile(executable) or not os.access(executable, os.X_OK):
            fail(f"{label} is not executable: {executable}")
    if not os.path.isdir(HELPER_PATH):
        fail(f"Git helper path does not exist: {HELPER_PATH}")
    if MAX_REQUEST_BYTES <= 0:
        fail("VEX_HTTP_MAX_REQUEST_BYTES must be positive")


def initialize_ready_repository() -> None:
    completed = subprocess.run(
        [INIT_REPOSITORY, READY_REPOSITORY],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=90,
    )
    if completed.returncode:
        detail = completed.stderr.strip() or completed.stdout.strip() or "unknown error"
        fail(f"could not initialize Vex storage readiness repository: {detail}")
    if completed.stdout.strip():
        print(completed.stdout.strip(), flush=True)


def make_server(port: int, tls: bool) -> VexMySQLS3HTTPServer:
    server = VexMySQLS3HTTPServer((BIND, port), VexMySQLS3HTTPRequestHandler)
    if tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(CERT, KEY)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    return server


def main() -> int:
    if not ROOT.is_dir():
        fail(f"GIT_HTTP_ROOT does not exist: {ROOT}")
    if not os.path.isfile(CERT) or not os.path.isfile(KEY):
        fail("local TLS certificate is missing; run scripts/vex/up.sh")
    require_configuration()
    initialize_ready_repository()

    servers = [make_server(HTTPS_PORT, True)]
    if ALLOW_HTTP:
        servers.append(make_server(HTTP_PORT, False))
    threads: list[threading.Thread] = []
    for server in servers:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        threads.append(thread)
    print(
        "vex mysql-s3-http: listening with custom Git "
        "(MySQL metadata, S3 object bytes)",
        flush=True,
    )

    stopped = threading.Event()

    def stop(_signum: int, _frame: object) -> None:
        if stopped.is_set():
            return
        stopped.set()
        for server in servers:
            server.shutdown()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    stopped.wait()
    for server in servers:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
