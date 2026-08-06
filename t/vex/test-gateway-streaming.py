"""Focused bounded-streaming checks for the Vex MySQL+S3 smart-HTTP gateway."""

import importlib.util
import io
import gzip
import os
import socket
import stat
import sys
import tempfile
import threading
from pathlib import Path


def load_gateway(path: str):
    spec = importlib.util.spec_from_file_location("vex_mysql_s3_http", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load gateway module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class TrackingReader(io.BytesIO):
    def __init__(self, payload: bytes):
        super().__init__(payload)
        self.maximum_request = 0

    def read(self, size: int = -1) -> bytes:
        self.maximum_request = max(self.maximum_request, size)
        return super().read(size)


class QuietStderr:
    def __init__(self):
        self.buffer = io.BytesIO()

    def write(self, value: str) -> int:
        return len(value)

    def flush(self) -> None:
        pass


def recv_until(client: socket.socket, marker: bytes) -> bytes:
    data = bytearray()
    while marker not in data:
        chunk = client.recv(65536)
        if not chunk:
            raise AssertionError(f"connection closed before {marker!r}: {data!r}")
        data.extend(chunk)
    return bytes(data)


def recv_all(client: socket.socket, initial: bytes = b"") -> bytes:
    data = bytearray(initial)
    while True:
        chunk = client.recv(65536)
        if not chunk:
            return bytes(data)
        data.extend(chunk)


def write_backend(path: Path) -> None:
    path.write_text(
        """#!{python}
import os
import sys

length = int(os.environ[\"CONTENT_LENGTH\"])
# More than a typical pipe capacity: the parent must drain stderr before the
# child can write its response header.
sys.stderr.buffer.write(b\"x\" * (256 * 1024))
sys.stderr.buffer.flush()
out = sys.stdout.buffer
out.write(b\"Status: 200 OK\\r\\nContent-Type: application/octet-stream\\r\\nX-Vex-Relay: streaming\\r\\n\\r\\n\")
out.write(b\"early:\")
out.flush()
received = 0
while received < length:
    block = sys.stdin.buffer.read(min(4096, length - received))
    if not block:
        break
    received += len(block)
out.write(b\"late:%d\" % received)
out.flush()
""".format(python=sys.executable),
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def request_content_length(port: int, payload: bytes) -> bytes:
    client = socket.create_connection(("127.0.0.1", port), timeout=5)
    client.settimeout(5)
    try:
        head = (
            b"POST /relay.git/git-upload-pack HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            + f"Content-Length: {len(payload)}\r\n".encode("ascii")
            + b"Content-Type: application/x-git-upload-pack-request\r\n\r\n"
        )
        first = 4096
        client.sendall(head + payload[:first])
        early = recv_until(client, b"early:")
        assert b"HTTP/1.1 200 OK" in early
        assert b"X-Vex-Relay: streaming" in early
        assert b"late:" not in early
        client.sendall(payload[first:])
        client.shutdown(socket.SHUT_WR)
        return recv_all(client, early)
    finally:
        client.close()


def request_chunked(port: int, payload: bytes) -> bytes:
    client = socket.create_connection(("127.0.0.1", port), timeout=5)
    client.settimeout(5)
    try:
        client.sendall(
            b"POST /relay.git/git-upload-pack HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Transfer-Encoding: chunked\r\n"
            b"Content-Type: application/x-git-upload-pack-request\r\n\r\n"
        )
        for offset in range(0, len(payload), 7919):
            chunk = payload[offset : offset + 7919]
            client.sendall(f"{len(chunk):X}\r\n".encode("ascii") + chunk + b"\r\n")
        client.sendall(b"0\r\nX-Test-Trailer: present\r\n\r\n")
        client.shutdown(socket.SHUT_WR)
        return recv_all(client)
    finally:
        client.close()


def request_gzip(port: int, payload: bytes) -> bytes:
    encoded = gzip.compress(payload)
    client = socket.create_connection(("127.0.0.1", port), timeout=5)
    client.settimeout(5)
    try:
        client.sendall(
            b"POST /relay.git/git-upload-pack HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            + f"Content-Length: {len(encoded)}\r\n".encode("ascii")
            + b"Content-Encoding: gzip\r\n"
            + b"Content-Type: application/x-git-upload-pack-request\r\n\r\n"
            + encoded
        )
        client.shutdown(socket.SHUT_WR)
        return recv_all(client)
    finally:
        client.close()


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test-gateway-streaming.py GATEWAY")
    gateway = load_gateway(sys.argv[1])

    # The low-level copier must never ask its source for a whole request.
    source = TrackingReader(b"streamed-body" * 10000)
    destination = io.BytesIO()
    copied = gateway.copy_request_body(source, destination, len(source.getvalue()))
    assert copied == len(destination.getvalue())
    assert source.maximum_request <= gateway.STREAM_CHUNK_BYTES

    status, headers, prefix = gateway.read_cgi_response_headers(
        io.BytesIO(b"Status: 201 Created\r\nContent-Type: text/plain\r\n\r\nfirst-pack-bytes")
    )
    assert status == 201
    assert ("Content-Type", "text/plain") in headers
    assert prefix == b"first-pack-bytes"
    try:
        gateway.read_cgi_response_headers(io.BytesIO(b"X-Long: " + b"x" * 80), max_header_bytes=64)
    except gateway.CGIResponseError:
        pass
    else:
        raise AssertionError("unbounded CGI header was accepted")

    with tempfile.TemporaryDirectory(prefix="vex-gateway-streaming-") as directory:
        backend = Path(directory) / "fake-http-backend.py"
        write_backend(backend)

        previous_backend = gateway.HTTP_BACKEND
        previous_repository_for_path = gateway.repository_for_path
        previous_require_auth = gateway.REQUIRE_PUSH_AUTH
        previous_stderr = gateway.sys.stderr
        captured_stderr = QuietStderr()
        gateway.HTTP_BACKEND = str(backend)
        gateway.REQUIRE_PUSH_AUTH = False
        gateway.sys.stderr = captured_stderr
        gateway.repository_for_path = lambda _path: gateway.VexRepository(
            name="relay.git",
            path=Path(directory),
            uri="vex://00000000000000000000000000000000",
            repo_id="00000000000000000000000000000000",
        )
        server = gateway.VexMySQLS3HTTPServer(
            ("127.0.0.1", 0), gateway.VexMySQLS3HTTPRequestHandler
        )
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            payload = bytes(range(251)) * 4096
            response = request_content_length(server.server_address[1], payload)
            assert response.endswith(f"late:{len(payload)}".encode("ascii"))
            chunked_response = request_chunked(server.server_address[1], payload[:65537])
            assert chunked_response.endswith(b"late:65537")
            gzip_response = request_gzip(server.server_address[1], payload[:65537])
            assert gzip_response.endswith(b"late:65537")
            assert len(captured_stderr.buffer.getvalue()) >= 2 * 256 * 1024
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
            gateway.HTTP_BACKEND = previous_backend
            gateway.repository_for_path = previous_repository_for_path
            gateway.REQUIRE_PUSH_AUTH = previous_require_auth
            gateway.sys.stderr = previous_stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
