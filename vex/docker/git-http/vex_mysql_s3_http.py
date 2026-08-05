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
import tempfile
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import BinaryIO, Iterable
from urllib.parse import unquote, urlsplit


ROOT = Path(os.environ.get("GIT_HTTP_ROOT", "/srv/git")).resolve()
BIND = os.environ.get("VEX_HTTP_BIND", "0.0.0.0")
HTTP_PORT = int(os.environ.get("VEX_HTTP_PORT", "8080"))
HTTPS_PORT = int(os.environ.get("VEX_HTTPS_PORT", "8443"))
ALLOW_HTTP = os.environ.get("GIT_ALLOW_HTTP", "1") == "1"
REQUIRE_PUSH_AUTH = os.environ.get("VEX_HTTP_REQUIRE_PUSH_AUTH", "1") == "1"
BASIC_USER = os.environ.get("VEX_HTTP_BASIC_USER", "vex")
BASIC_PASSWORD = os.environ.get("VEX_HTTP_BASIC_PASSWORD", "")
# A git/git mirror import is materially larger than the original 64 MiB
# bootstrap limit.  The request is relayed incrementally below, so this is a
# disk/network admission limit rather than a RAM allocation.  Operators can
# still lower it for a tightly bounded deployment.
DEFAULT_MAX_REQUEST_BYTES = 8 * 1024 * 1024 * 1024
MAX_REQUEST_BYTES = int(os.environ.get("VEX_HTTP_MAX_REQUEST_BYTES", str(DEFAULT_MAX_REQUEST_BYTES)))
MAX_CGI_HEADER_BYTES = int(os.environ.get("VEX_HTTP_MAX_CGI_HEADER_BYTES", "65536"))
MAX_CHUNK_LINE_BYTES = int(os.environ.get("VEX_HTTP_MAX_CHUNK_LINE_BYTES", "8192"))
MAX_CHUNK_TRAILER_BYTES = int(os.environ.get("VEX_HTTP_MAX_CHUNK_TRAILER_BYTES", "65536"))
STREAM_CHUNK_BYTES = int(os.environ.get("VEX_HTTP_STREAM_CHUNK_BYTES", "65536"))
CGI_TIMEOUT_SECONDS = int(os.environ.get("VEX_HTTP_CGI_TIMEOUT_SECONDS", "300"))
IO_TIMEOUT_SECONDS = int(os.environ.get("VEX_HTTP_IO_TIMEOUT_SECONDS", "300"))
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
CGI_HOP_BY_HOP_HEADERS = frozenset(
    {
        "connection",
        "keep-alive",
        "proxy-authenticate",
        "proxy-authorization",
        "te",
        "trailer",
        "transfer-encoding",
        "upgrade",
    }
)
HTTP_TOKEN_RE = re.compile(r"[!#$%&'*+\-.^_`|~0-9A-Za-z]+\Z")


class RequestBodyError(Exception):
    """A client body did not match the declared safe HTTP framing."""


class CGIResponseError(Exception):
    """The CGI child did not produce a bounded, valid response header."""


@dataclass
class RequestBody:
    """A decoded request source which can be copied to CGI incrementally."""

    length: int
    source: BinaryIO
    close_source: bool = False

    def close(self) -> None:
        if self.close_source:
            self.source.close()


@dataclass
class StdinPumpResult:
    """Outcome shared between the request relay and the response relay."""

    bytes_copied: int = 0
    error: BaseException | None = None
    done: threading.Event = field(default_factory=threading.Event)


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


def _malformed_cgi_response() -> CGIResponseError:
    return CGIResponseError("git-http-backend returned malformed CGI output")


def parse_cgi_headers(header_bytes: bytes) -> tuple[int, list[tuple[str, str]]]:
    """Validate the bounded CGI header block before sending any HTTP bytes."""
    if not header_bytes:
        raise _malformed_cgi_response()

    lines = header_bytes.split(b"\r\n") if b"\r\n" in header_bytes else header_bytes.split(b"\n")
    status = 200
    headers: list[tuple[str, str]] = []
    content_length: str | None = None
    for line in lines:
        if not line or b":" not in line:
            raise _malformed_cgi_response()
        name, value = line.split(b":", 1)
        try:
            name_text = name.decode("ascii")
            value_text = value.decode("latin-1").strip()
        except UnicodeDecodeError as error:
            raise _malformed_cgi_response() from error
        if not HTTP_TOKEN_RE.fullmatch(name_text):
            raise _malformed_cgi_response()
        if any(ord(character) < 32 and character != "\t" or ord(character) == 127 for character in value_text):
            raise _malformed_cgi_response()

        lowered = name_text.lower()
        if lowered == "status":
            try:
                status = int(value_text.split(None, 1)[0])
            except (IndexError, ValueError) as error:
                raise _malformed_cgi_response() from error
            if status < 100 or status > 599:
                raise _malformed_cgi_response()
            continue
        if lowered in CGI_HOP_BY_HOP_HEADERS:
            continue
        if lowered == "content-length":
            if not value_text.isdecimal():
                raise _malformed_cgi_response()
            if content_length is not None and content_length != value_text:
                raise _malformed_cgi_response()
            content_length = value_text
        headers.append((name_text, value_text))
    return status, headers


def split_cgi_response(raw: bytes) -> tuple[int, list[tuple[str, str]], bytes]:
    """Return a validated CGI status, headers, and body for small callers.

    The gateway itself uses :func:`read_cgi_response_headers` so a large pack
    body is never accumulated here.  Retaining this helper makes the malformed
    CGI contract easy to exercise in a focused test.
    """
    if b"\r\n\r\n" in raw:
        header_bytes, body = raw.split(b"\r\n\r\n", 1)
    elif b"\n\n" in raw:
        header_bytes, body = raw.split(b"\n\n", 1)
    else:
        return (
            502,
            [("Content-Type", "text/plain; charset=utf-8")],
            b"git-http-backend returned malformed CGI output\n",
        )
    try:
        status, headers = parse_cgi_headers(header_bytes)
    except CGIResponseError:
        return (
            502,
            [("Content-Type", "text/plain; charset=utf-8")],
            b"git-http-backend returned malformed CGI output\n",
        )
    return status, headers, body


def read_cgi_response_headers(
    stream: BinaryIO,
    max_header_bytes: int = MAX_CGI_HEADER_BYTES,
) -> tuple[int, list[tuple[str, str]], bytes]:
    """Read exactly one bounded CGI header block and retain any body prefix.

    ``BufferedReader.read1`` avoids waiting for an arbitrary pack-sized read.
    It can still return bytes after the header delimiter, which are returned as
    ``body_prefix`` and relayed before subsequent stream reads.
    """
    raw = bytearray()
    reader = getattr(stream, "read1", stream.read)
    while True:
        crlf_end = raw.find(b"\r\n\r\n")
        lf_end = raw.find(b"\n\n")
        if crlf_end >= 0 and (lf_end < 0 or crlf_end <= lf_end):
            delimiter_length = 4
            header_end = crlf_end
        elif lf_end >= 0:
            delimiter_length = 2
            header_end = lf_end
        else:
            if len(raw) > max_header_bytes:
                raise _malformed_cgi_response()
            chunk = reader(min(STREAM_CHUNK_BYTES, max_header_bytes + 4 - len(raw)))
            if not chunk:
                raise _malformed_cgi_response()
            raw.extend(chunk)
            continue

        if header_end > max_header_bytes:
            raise _malformed_cgi_response()
        status, headers = parse_cgi_headers(bytes(raw[:header_end]))
        return status, headers, bytes(raw[header_end + delimiter_length :])


def copy_request_body(source: BinaryIO, destination: BinaryIO, length: int) -> int:
    """Copy a declared request length without creating a whole-body buffer."""
    remaining = length
    copied = 0
    while remaining:
        chunk = source.read(min(STREAM_CHUNK_BYTES, remaining))
        if not chunk:
            raise RequestBodyError("truncated request body")
        destination.write(chunk)
        destination.flush()
        copied += len(chunk)
        remaining -= len(chunk)
    return copied


def drain_child_stderr(stream: BinaryIO) -> None:
    """Relay CGI diagnostics continuously so stderr can never fill its pipe."""
    while True:
        chunk = stream.read(STREAM_CHUNK_BYTES)
        if not chunk:
            return
        destination = getattr(sys.stderr, "buffer", None)
        if destination is not None:
            destination.write(chunk)
            destination.flush()
        else:
            sys.stderr.write(chunk.decode("utf-8", "replace"))
            sys.stderr.flush()


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

    def setup(self) -> None:
        super().setup()
        # The relay has bounded byte and header limits, but a client must also
        # make progress while it holds a worker thread or a CGI stdin pipe.
        self.connection.settimeout(IO_TIMEOUT_SECONDS)

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

    def _request_error(self, status: int, message: str) -> None:
        """Return a framing error and make unread client bytes harmless."""
        self.close_connection = True
        self.send_error(status, message)

    def _read_chunk_line(self) -> bytes:
        line = self.rfile.readline(MAX_CHUNK_LINE_BYTES + 1)
        if not line or len(line) > MAX_CHUNK_LINE_BYTES or not line.endswith(b"\r\n"):
            raise RequestBodyError("invalid chunked request")
        return line[:-2]

    def _read_chunked_body(self) -> RequestBody | None:
        """Decode a chunked request into a bounded disk-backed source.

        CGI needs a concrete ``CONTENT_LENGTH``.  Spooling only this uncommon
        framing keeps normal smart-HTTP Content-Length requests fully
        streaming while preserving safe chunked support without assembling a
        multi-gigabyte Python ``bytes`` object.
        """
        spool = tempfile.TemporaryFile(mode="w+b")
        length = 0
        try:
            while True:
                raw_size = self._read_chunk_line().split(b";", 1)[0]
                if not raw_size or not all(character in b"0123456789abcdefABCDEF" for character in raw_size):
                    raise RequestBodyError("invalid chunked request")
                chunk_length = int(raw_size, 16)
                if chunk_length == 0:
                    trailer_length = 0
                    while True:
                        trailer = self._read_chunk_line()
                        if not trailer:
                            spool.seek(0)
                            return RequestBody(length, spool, close_source=True)
                        trailer_length += len(trailer) + 2
                        if trailer_length > MAX_CHUNK_TRAILER_BYTES:
                            raise RequestBodyError("chunked request trailers exceed Vex HTTP limit")
                if chunk_length > MAX_REQUEST_BYTES - length:
                    self._request_error(413, "request exceeds Vex HTTP limit")
                    spool.close()
                    return None
                remaining = chunk_length
                while remaining:
                    chunk = self.rfile.read(min(STREAM_CHUNK_BYTES, remaining))
                    if not chunk:
                        raise RequestBodyError("truncated chunked request")
                    spool.write(chunk)
                    length += len(chunk)
                    remaining -= len(chunk)
                if self.rfile.read(2) != b"\r\n":
                    raise RequestBodyError("truncated chunked request")
        except RequestBodyError as error:
            spool.close()
            self._request_error(400, str(error))
            return None

    def _request_body(self) -> RequestBody | None:
        """Return a validated source rather than reading a whole POST body."""
        if self.command != "POST":
            return RequestBody(0, self.rfile)

        transfer_values = self.headers.get_all("Transfer-Encoding", [])
        transfer_codings = [
            coding.strip().lower()
            for value in transfer_values
            for coding in value.split(",")
            if coding.strip()
        ]
        content_lengths = self.headers.get_all("Content-Length", [])
        if transfer_codings:
            if content_lengths:
                self._request_error(400, "ambiguous request framing")
                return None
            if transfer_codings != ["chunked"]:
                self._request_error(501, "unsupported Transfer-Encoding")
                return None
            return self._read_chunked_body()

        if len(content_lengths) != 1:
            self._request_error(411, "Content-Length is required")
            return None
        raw_length = content_lengths[0].strip()
        if not raw_length.isdecimal():
            self._request_error(400, "invalid Content-Length")
            return None
        length = int(raw_length)
        if length > MAX_REQUEST_BYTES:
            self._request_error(413, "request exceeds Vex HTTP limit")
            return None
        return RequestBody(length, self.rfile)

    def _send_backend_error(self, status: int, message: str) -> None:
        body = (message + "\n").encode("utf-8")
        self.close_connection = True
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    @staticmethod
    def _stop_backend(process: subprocess.Popen[bytes]) -> None:
        try:
            if process.stdin is not None:
                process.stdin.close()
        except OSError:
            pass
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()

    def _relay_backend(self, environ: dict[str, str], body: RequestBody) -> None:
        """Pump request, stderr, and response concurrently with bounded RAM."""
        try:
            process = subprocess.Popen(
                [HTTP_BACKEND],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environ,
            )
        except OSError as error:
            body.close()
            sys.stderr.write(f"vex mysql-s3-http: could not start git-http-backend: {error}\n")
            self._send_backend_error(502, "could not start git-http-backend")
            return

        if process.stdin is None or process.stdout is None or process.stderr is None:
            body.close()
            self._stop_backend(process)
            self._send_backend_error(502, "git-http-backend pipes are unavailable")
            return

        pump = StdinPumpResult()

        def pump_stdin() -> None:
            try:
                pump.bytes_copied = copy_request_body(body.source, process.stdin, body.length)
            except (BrokenPipeError, ConnectionResetError, OSError, ValueError, RequestBodyError) as error:
                pump.error = error
            finally:
                try:
                    process.stdin.close()
                except (BrokenPipeError, OSError, ValueError):
                    pass
                body.close()
                pump.done.set()

        stderr_thread = threading.Thread(
            target=drain_child_stderr,
            args=(process.stderr,),
            name="vex-git-http-stderr",
            daemon=True,
        )
        stdin_thread = threading.Thread(
            target=pump_stdin,
            name="vex-git-http-stdin",
            daemon=True,
        )
        timed_out = threading.Event()

        def timeout_backend() -> None:
            if process.poll() is None:
                timed_out.set()
                process.kill()

        timeout = threading.Timer(CGI_TIMEOUT_SECONDS, timeout_backend)
        timeout.daemon = True
        stderr_thread.start()
        stdin_thread.start()
        timeout.start()

        response_started = False
        try:
            try:
                status, headers, body_prefix = read_cgi_response_headers(process.stdout)
            except CGIResponseError:
                self._stop_backend(process)
                if isinstance(pump.error, RequestBodyError):
                    self._send_backend_error(400, "truncated request body")
                elif timed_out.is_set():
                    self._send_backend_error(504, "git-http-backend timed out")
                else:
                    self._send_backend_error(502, "git-http-backend returned malformed CGI output")
                return

            # A short Content-Length body is detected by the concurrent pump.
            # If it failed before CGI committed a response, reject it rather
            # than forwarding a potentially misleading backend error.
            if pump.done.is_set() and isinstance(pump.error, RequestBodyError):
                self._stop_backend(process)
                self._send_backend_error(400, "truncated request body")
                return

            self.send_response(status)
            for name, value in headers:
                self.send_header(name, value)
            # Without a CGI Content-Length, EOF framing is correct because the
            # listener closes this connection after the stream completes.
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            response_started = True
            self.wfile.flush()

            if self.command != "HEAD" and body_prefix:
                self.wfile.write(body_prefix)
            while True:
                chunk = process.stdout.read(STREAM_CHUNK_BYTES)
                if not chunk:
                    break
                if self.command != "HEAD":
                    self.wfile.write(chunk)
            self.wfile.flush()
            process.wait()
            if process.returncode:
                sys.stderr.write(
                    "vex mysql-s3-http: git-http-backend exited with "
                    f"status {process.returncode}\n"
                )
        except (BrokenPipeError, ConnectionResetError, OSError):
            # The client may disconnect while a pack is in flight.  Once the
            # response has started there is no valid HTTP error to append.
            if not response_started:
                self._send_backend_error(502, "could not relay git-http-backend response")
            self._stop_backend(process)
        finally:
            timeout.cancel()
            if process.poll() is None:
                self._stop_backend(process)
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
            stderr_thread.join(timeout=1)

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
        body = self._request_body()
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
                "CONTENT_LENGTH": str(body.length),
                "REMOTE_ADDR": self.client_address[0],
            }
        )
        environ["PATH"] = HELPER_PATH + os.pathsep + environ.get("PATH", "")
        set_cgi_git_protocol(environ, self.headers)
        if self._authorized():
            environ["REMOTE_USER"] = BASIC_USER
        self._relay_backend(environ, body)


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
    configured_limits = {
        "VEX_HTTP_MAX_REQUEST_BYTES": MAX_REQUEST_BYTES,
        "VEX_HTTP_MAX_CGI_HEADER_BYTES": MAX_CGI_HEADER_BYTES,
        "VEX_HTTP_MAX_CHUNK_LINE_BYTES": MAX_CHUNK_LINE_BYTES,
        "VEX_HTTP_MAX_CHUNK_TRAILER_BYTES": MAX_CHUNK_TRAILER_BYTES,
        "VEX_HTTP_STREAM_CHUNK_BYTES": STREAM_CHUNK_BYTES,
        "VEX_HTTP_CGI_TIMEOUT_SECONDS": CGI_TIMEOUT_SECONDS,
        "VEX_HTTP_IO_TIMEOUT_SECONDS": IO_TIMEOUT_SECONDS,
    }
    invalid = [name for name, value in configured_limits.items() if value <= 0]
    if invalid:
        fail("gateway limits must be positive: " + ", ".join(invalid))


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
