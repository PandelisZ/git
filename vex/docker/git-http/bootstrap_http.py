#!/usr/bin/env python3
"""Small, deliberately-labelled stock-Git smart-HTTP bootstrap.

This is not a Vex storage implementation.  It is a long-lived HTTP listener
which invokes the stock ``git http-backend`` against a classic-bare volume so
the Compose topology can prove TLS and stock-client plumbing before the Vex
ODB/ref server binary is available.
"""

from __future__ import annotations

import base64
import hmac
import json
import os
import signal
import ssl
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Dict, Iterable, Tuple
from urllib.parse import unquote, urlsplit


ROOT = os.environ.get("GIT_HTTP_ROOT", "/srv/git")
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


def split_cgi_response(raw: bytes) -> Tuple[int, Iterable[Tuple[str, str]], bytes]:
    """Return a validated CGI status, headers, and body."""
    if b"\r\n\r\n" in raw:
        header_bytes, body = raw.split(b"\r\n\r\n", 1)
        lines = header_bytes.split(b"\r\n")
    elif b"\n\n" in raw:
        header_bytes, body = raw.split(b"\n\n", 1)
        lines = header_bytes.split(b"\n")
    else:
        return 502, (("Content-Type", "text/plain; charset=utf-8"),), b"git-http-backend returned malformed CGI output\n"

    status = 200
    headers = []
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


class GitHTTPHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "VexGitHTTPBootstrap/0"

    def log_message(self, fmt: str, *args: object) -> None:
        sys.stderr.write("vex git-http: %s\n" % (fmt % args))

    def do_GET(self) -> None:
        self._serve_git()

    def do_HEAD(self) -> None:
        self._serve_git()

    def do_POST(self) -> None:
        self._serve_git()

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Allow", "GET, HEAD, POST, OPTIONS")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _healthz(self) -> None:
        payload = {
            "service": "git-http",
            "mode": "bootstrap-classic-bare",
            "vex_storage_active": False,
            "message": "stock Git smart-HTTP plumbing only; MySQL and MinIO are not Git storage in this mode",
        }
        body = (json.dumps(payload, sort_keys=True) + "\n").encode("utf-8")
        self.send_response(200)
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

    def _read_body(self) -> bytes | None:
        if self.command != "POST":
            return b""
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
            self.send_error(413, "request exceeds bootstrap size limit")
            return None
        return self.rfile.read(length)

    def _serve_git(self) -> None:
        split = urlsplit(self.path)
        if split.path == "/healthz":
            self._healthz()
            return
        if not ALLOW_HTTP and not isinstance(self.connection, ssl.SSLSocket):
            self.send_error(404)
            return

        decoded_path = unquote(split.path)
        parts = [part for part in decoded_path.split("/") if part]
        if ".." in parts or "\x00" in decoded_path:
            self.send_error(400, "unsafe Git path")
            return
        if self._needs_push_auth(split.path, split.query) and not self._authorized():
            self._auth_required()
            return

        body = self._read_body()
        if body is None:
            return
        environ: Dict[str, str] = dict(os.environ)
        environ.update(
            {
                "GIT_PROJECT_ROOT": ROOT,
                "GIT_HTTP_EXPORT_ALL": "1",
                "PATH_INFO": decoded_path,
                "QUERY_STRING": split.query,
                "REQUEST_METHOD": self.command,
                "CONTENT_TYPE": self.headers.get("Content-Type", ""),
                "CONTENT_LENGTH": str(len(body)),
                "REMOTE_ADDR": self.client_address[0],
            }
        )
        if self._authorized():
            environ["REMOTE_USER"] = BASIC_USER
        try:
            completed = subprocess.run(
                ["git", "http-backend"],
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


def make_server(port: int, tls: bool) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer((BIND, port), GitHTTPHandler)
    if tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(CERT, KEY)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    return server


def main() -> int:
    if not os.path.isdir(ROOT):
        raise SystemExit("git-http bootstrap: GIT_HTTP_ROOT does not exist: %s" % ROOT)
    if not os.path.isfile(CERT) or not os.path.isfile(KEY):
        raise SystemExit("git-http bootstrap: local TLS certificate is missing; run scripts/vex/up.sh")

    servers = [make_server(HTTPS_PORT, True)]
    if ALLOW_HTTP:
        servers.append(make_server(HTTP_PORT, False))
    threads = []
    for server in servers:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        threads.append(thread)

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
