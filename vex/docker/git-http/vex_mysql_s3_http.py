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
import stat
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
# The classic repository used by the clone benchmark deliberately lives outside
# ``ROOT``.  ``known_repositories()`` walks ROOT for thin Vex repositories as
# part of the MySQL+S3 health response; putting a full git/git control there
# would turn that small health probe into an object-tree traversal.
BENCH_DISK_ROOT = Path(os.environ.get("VEX_BENCH_DISK_ROOT", "/srv/vex-bench-disk")).resolve()
BENCH_DISK_NAMESPACE = "/__vex_bench_disk__"
BENCH_DISK_REPOSITORY = "git-git-disk.git"
BENCH_DISK_ROUTE = f"{BENCH_DISK_NAMESPACE}/{BENCH_DISK_REPOSITORY}"
BENCH_DISK_MARKER = "vex.benchmarkcontrol"
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
# A receive-pack result is control/status data, never a clone pack. Buffer it
# until CGI exits so a killed receive worker becomes a prompt HTTP failure
# instead of a syntactically incomplete smart-HTTP result that can leave a
# stock client waiting for report-status forever.
MAX_RECEIVE_RESPONSE_BYTES = int(
    os.environ.get("VEX_HTTP_MAX_RECEIVE_RESPONSE_BYTES", str(8 * 1024 * 1024))
)
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

# ``/healthz`` exposes a convenience inventory, but it must never turn that
# observability detail into a second, unbounded Git workload.  In particular,
# a large repository can contain many object directories and an interrupted
# receive can make a custom-Git config lookup wait for recovery.  These bounds
# apply only to the health inventory; normal smart-HTTP routing below retains
# its authoritative custom-Git validation.
HEALTH_SCAN_MAX_REPOSITORIES = max(
    1, int(os.environ.get("VEX_HTTP_HEALTH_SCAN_MAX_REPOSITORIES", "64"))
)
HEALTH_SCAN_MAX_DIRECTORIES = max(
    1, int(os.environ.get("VEX_HTTP_HEALTH_SCAN_MAX_DIRECTORIES", "256"))
)
HEALTH_SCAN_MAX_DIRECTORY_ENTRIES = max(
    1, int(os.environ.get("VEX_HTTP_HEALTH_SCAN_MAX_DIRECTORY_ENTRIES", "1024"))
)
HEALTH_SCAN_MAX_DEPTH = max(1, int(os.environ.get("VEX_HTTP_HEALTH_SCAN_MAX_DEPTH", "8")))
HEALTH_SCAN_MAX_CONFIG_BYTES = max(
    1, int(os.environ.get("VEX_HTTP_HEALTH_SCAN_MAX_CONFIG_BYTES", str(64 * 1024)))
)

REPO_COMPONENT_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\Z")
VEX_URI_RE = re.compile(r"vex://([0-9a-fA-F]{32})\Z")
HEALTH_CONFIG_EXTENSIONS_SECTION_RE = re.compile(
    rb"[ \t]*\[[ \t]*(?i:extensions)[ \t]*\][ \t]*(?:[;#].*)?\Z"
)
HEALTH_CONFIG_REFSTORAGE_RE = re.compile(
    rb"[ \t]*(?i:refstorage)[ \t]*=[ \t]*(vex://[0-9a-fA-F]{32})[ \t]*(?:[;#].*)?\Z"
)
HEALTH_CONFIG_INCLUDE_SECTION_RE = re.compile(
    rb"[ \t]*\[[ \t]*(?i:include)(?:(?i:if))?(?:[ \t]+[^\]]+)?\][ \t]*(?:[;#].*)?\Z"
)
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


@dataclass(frozen=True)
class BenchmarkDiskRepository:
    """The one explicit classic-bare control permitted by the gateway.

    This is intentionally not a general classic-Git fallback.  It is mounted
    in a separate Compose volume and gets a different CGI project root, so a
    normal Vex URL can never resolve to its on-disk object bytes.
    """

    path: Path
    path_info: str


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


def benchmark_disk_namespace_requested(path: str) -> bool:
    """Whether a URL addresses the reserved benchmark-control namespace.

    A malformed request inside this namespace must not fall through to the
    Vex resolver: that would make a future Vex repository with the same path
    unexpectedly change the benchmark-control boundary.
    """
    decoded = unquote(path)
    return decoded == BENCH_DISK_NAMESPACE or decoded.startswith(BENCH_DISK_NAMESPACE + "/")


def _git_config_value(git_dir: Path, key: str) -> str | None:
    """Read one local repository config value, failing closed on any error."""
    try:
        completed = subprocess.run(
            [GIT, f"--git-dir={git_dir}", "config", "--get", key],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if completed.returncode:
        return None
    return completed.stdout.strip()


def benchmark_disk_repository_for_request(
    path: str,
    method: str,
    query: str,
) -> BenchmarkDiskRepository | None:
    """Resolve the one authenticated smart-HTTP benchmark disk control.

    Only the stock smart-HTTP discovery and RPC endpoints are accepted.  In
    particular, this gateway does not expose arbitrary classic repositories,
    object files, configs, or paths beneath the mounted control volume.
    """
    decoded = unquote(path)
    if decoded == BENCH_DISK_ROUTE + "/info/refs":
        if method not in {"GET", "HEAD"} or query not in {
            "service=git-upload-pack",
            "service=git-receive-pack",
        }:
            return None
        path_info = f"/{BENCH_DISK_REPOSITORY}/info/refs"
    elif decoded == BENCH_DISK_ROUTE + "/git-upload-pack":
        if method != "POST" or query:
            return None
        path_info = f"/{BENCH_DISK_REPOSITORY}/git-upload-pack"
    elif decoded == BENCH_DISK_ROUTE + "/git-receive-pack":
        if method != "POST" or query:
            return None
        path_info = f"/{BENCH_DISK_REPOSITORY}/git-receive-pack"
    else:
        return None

    try:
        root = BENCH_DISK_ROOT.resolve()
        candidate = (root / BENCH_DISK_REPOSITORY).resolve()
        candidate.relative_to(root)
    except (OSError, RuntimeError, ValueError):
        return None
    if not candidate.is_dir() or not (candidate / "HEAD").is_file() or not (candidate / "objects").is_dir():
        return None

    # A seeded classic control must opt in explicitly.  Reject a Vex thin
    # repository even if it accidentally lands in the separate volume.
    if _git_config_value(candidate, BENCH_DISK_MARKER) != "true":
        return None
    if _git_config_value(candidate, "extensions.refstorage") is not None:
        return None
    return BenchmarkDiskRepository(path=candidate, path_info=path_info)


def repository_for_request(
    path: str,
    method: str,
    query: str,
) -> tuple[VexRepository | BenchmarkDiskRepository | None, bool]:
    """Resolve a request and identify the reserved disk namespace.

    The boolean records that the request entered the reserved namespace even
    if it was malformed or the control is absent.  Callers use it to return a
    terminal 404 instead of attempting the normal Vex path resolver.
    """
    if benchmark_disk_namespace_requested(path):
        return benchmark_disk_repository_for_request(path, method, query), True
    return repository_for_path(path), False


def cgi_environment_for_repository(
    repository: VexRepository | BenchmarkDiskRepository,
    path: str,
    query: str,
    method: str,
    content_type: str,
    content_length: int,
    remote_addr: str,
) -> dict[str, str]:
    """Build a clean request-scoped CGI environment for one routed repository."""
    environ = dict(os.environ)
    for key in (
        "GIT_DIR",
        "GIT_WORK_TREE",
        "GIT_INDEX_FILE",
        "GIT_OBJECT_DIRECTORY",
        "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_REFERENCE_BACKEND",
    ):
        environ.pop(key, None)

    if isinstance(repository, VexRepository):
        project_root = ROOT
        path_info = unquote(path)
        environ["GIT_REFERENCE_BACKEND"] = repository.uri
    else:
        project_root = BENCH_DISK_ROOT
        path_info = repository.path_info

    environ.update(
        {
            "GIT_EXEC_PATH": GIT_EXEC_PATH,
            "GIT_HTTP_EXPORT_ALL": "1",
            "GIT_PROJECT_ROOT": str(project_root),
            "PATH_INFO": path_info,
            "QUERY_STRING": query,
            "REQUEST_METHOD": method,
            "CONTENT_TYPE": content_type,
            "CONTENT_LENGTH": str(content_length),
            "REMOTE_ADDR": remote_addr,
        }
    )
    environ["PATH"] = HELPER_PATH + os.pathsep + environ.get("PATH", "")
    return environ


def _regular_file(path: Path) -> bool:
    """Whether ``path`` is a regular file without following a symlink."""
    try:
        return stat.S_ISREG(path.stat(follow_symlinks=False).st_mode)
    except OSError:
        return False


def _read_bounded_regular_file(path: Path, maximum_bytes: int) -> bytes | None:
    """Read a small regular file without traversing a symlink.

    The health inventory only needs the local thin-repository ``config``
    scaffold.  Reading it directly is deliberately less capable than Git's
    configuration resolver: it does not follow includes, configs outside the
    repository, or symlinks.  That makes it a safe, bounded observability
    probe instead of another route-validation path.
    """
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError:
        return None

    try:
        file_stat = os.fstat(descriptor)
        if not stat.S_ISREG(file_stat.st_mode) or file_stat.st_size > maximum_bytes:
            return None

        content = bytearray()
        while len(content) <= maximum_bytes:
            chunk = os.read(descriptor, min(8192, maximum_bytes + 1 - len(content)))
            if not chunk:
                break
            content.extend(chunk)
        if len(content) > maximum_bytes:
            return None
        return bytes(content)
    except OSError:
        return None
    finally:
        os.close(descriptor)


def health_repository_uri(repository: Path) -> str | None:
    """Read one canonical local Vex URI for the bounded health inventory.

    This compact parser intentionally recognizes only the plain local config
    spelling emitted by ``git config``.  It rejects includes, duplicate or
    non-canonical values, and anything which would need Git's full config
    semantics.  Omission from a health response is harmless; accepting an
    ambiguous repository would be misleading.  Request routing continues to
    call :func:`repository_for_path`, which remains the authoritative check.
    """
    if not _regular_file(repository / "HEAD"):
        return None
    config = _read_bounded_regular_file(repository / "config", HEALTH_SCAN_MAX_CONFIG_BYTES)
    if config is None:
        return None

    in_extensions = False
    values: list[str] = []
    for raw_line in config.splitlines():
        line = raw_line.rstrip(b"\r")
        stripped = line.lstrip()
        if not stripped or stripped.startswith((b"#", b";")):
            continue
        if HEALTH_CONFIG_INCLUDE_SECTION_RE.fullmatch(line):
            # Includes can add or override refstorage values outside the
            # bounded local file.  Do not attempt a partial interpretation.
            return None
        if HEALTH_CONFIG_EXTENSIONS_SECTION_RE.fullmatch(line):
            in_extensions = True
            continue
        if stripped.startswith(b"["):
            in_extensions = False
            continue
        if not in_extensions:
            continue

        match = HEALTH_CONFIG_REFSTORAGE_RE.fullmatch(line)
        if match is not None:
            values.append(match.group(1).decode("ascii"))
            continue
        # A refstorage-looking line with syntax this small parser cannot
        # unambiguously interpret must fail closed rather than be guessed.
        if b"refstorage" in line.lower():
            return None

    if len(values) != 1:
        return None
    uri = values[0]
    return uri if VEX_URI_RE.fullmatch(uri) else None


def health_repository_candidates() -> Iterable[tuple[str, Path]]:
    """Yield a bounded set of direct ``*.git`` children beneath ``ROOT``.

    Unlike ``Path.rglob()``, this walker never descends into a candidate bare
    repository.  It also bounds traversal depth, directories, and entries per
    directory so a malformed or unexpectedly large filesystem cannot delay
    the health endpoint.  Symlinks are not followed.
    """
    if not ROOT.is_dir():
        return

    pending: list[tuple[Path, tuple[str, ...], int]] = [(ROOT, (), 0)]
    scanned_directories = 0
    yielded_repositories = 0
    while pending and scanned_directories < HEALTH_SCAN_MAX_DIRECTORIES:
        directory, relative_parts, depth = pending.pop()
        scanned_directories += 1
        try:
            iterator = os.scandir(directory)
        except OSError:
            continue
        with iterator:
            entries_seen = 0
            for entry in iterator:
                entries_seen += 1
                if entries_seen > HEALTH_SCAN_MAX_DIRECTORY_ENTRIES:
                    break
                try:
                    is_directory = entry.is_dir(follow_symlinks=False)
                except OSError:
                    continue
                if not is_directory or not REPO_COMPONENT_RE.fullmatch(entry.name):
                    continue

                candidate_depth = depth + 1
                if candidate_depth > HEALTH_SCAN_MAX_DEPTH:
                    continue
                candidate_parts = (*relative_parts, entry.name)
                candidate = Path(entry.path)
                if entry.name.endswith(".git"):
                    if yielded_repositories >= HEALTH_SCAN_MAX_REPOSITORIES:
                        return
                    yielded_repositories += 1
                    yield "/".join(candidate_parts), candidate
                    # A bare repository can contain an arbitrarily large
                    # object tree.  Never recurse beneath it, even if its
                    # local config is malformed or its receive is recovering.
                    continue
                if candidate_depth < HEALTH_SCAN_MAX_DEPTH:
                    pending.append((candidate, candidate_parts, candidate_depth))


def known_repositories() -> dict[str, str]:
    """Return a bounded, config-file-only inventory for ``/healthz``.

    This is intentionally an observability aid, not an authorization or
    routing decision.  It must stay independent from custom Git so one
    recovering repository cannot stall health for every other repository.
    """
    result: dict[str, str] = {}
    for name, candidate in health_repository_candidates():
        uri = health_repository_uri(candidate)
        if uri is None:
            continue
        match = VEX_URI_RE.fullmatch(uri)
        if match is not None:
            result[name] = match.group(1).lower()
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

    def _auth_required(self, purpose: str = "git-receive-pack") -> None:
        body = f"authentication required for {purpose}\n".encode("utf-8")
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
        buffer_receive_response = environ.get("PATH_INFO", "").endswith("/git-receive-pack")
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

            if buffer_receive_response:
                response_body = bytearray(body_prefix)
                if len(response_body) > MAX_RECEIVE_RESPONSE_BYTES:
                    self._stop_backend(process)
                    self._send_backend_error(502, "git-receive-pack response exceeds Vex limit")
                    return
                while True:
                    chunk = process.stdout.read(STREAM_CHUNK_BYTES)
                    if not chunk:
                        break
                    if len(response_body) + len(chunk) > MAX_RECEIVE_RESPONSE_BYTES:
                        self._stop_backend(process)
                        self._send_backend_error(502, "git-receive-pack response exceeds Vex limit")
                        return
                    response_body.extend(chunk)
                process.wait()
                if process.returncode:
                    sys.stderr.write(
                        "vex mysql-s3-http: git-http-backend exited with "
                        f"status {process.returncode}\n"
                    )
                    self._send_backend_error(
                        504 if timed_out.is_set() else 502,
                        "git-receive-pack timed out" if timed_out.is_set()
                        else "git-receive-pack failed",
                    )
                    return

                self.send_response(status)
                for name, value in headers:
                    self.send_header(name, value)
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True
                response_started = True
                if self.command != "HEAD" and response_body:
                    self.wfile.write(response_body)
                self.wfile.flush()
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

        repository, benchmark_disk_namespace = repository_for_request(
            split.path,
            self.command,
            split.query,
        )
        if repository is None:
            self.send_error(
                404,
                "unknown benchmark disk control" if benchmark_disk_namespace else "unknown Vex repository",
            )
            return
        if isinstance(repository, BenchmarkDiskRepository):
            # This route is an explicit benchmark fixture.  Unlike normal
            # read-only Vex fetches it is never anonymously exposed, and the
            # same authenticated control is used by fetch-after-push tests.
            if not self._authorized():
                self._auth_required("Vex benchmark disk control")
                return
        elif self._needs_push_auth(split.path, split.query) and not self._authorized():
            self._auth_required()
            return
        body = self._request_body()
        if body is None:
            return

        environ = cgi_environment_for_repository(
            repository,
            split.path,
            split.query,
            self.command,
            self.headers.get("Content-Type", ""),
            body.length,
            self.client_address[0],
        )
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
        "VEX_HTTP_MAX_RECEIVE_RESPONSE_BYTES": MAX_RECEIVE_RESPONSE_BYTES,
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
