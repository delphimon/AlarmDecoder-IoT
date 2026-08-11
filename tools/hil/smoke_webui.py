#!/usr/bin/env python3
"""Read-only HTTPS/WSS smoke test for a running AD2IoT device.

Credentials are loaded from a local ad2iot.ini and are never printed. The test
keeps a WebSocket open while it serializes REST requests over one persistent
connection, matching the browser's two-session TLS workload. It does not send
alarm-panel keys, restart commands, or firmware-update commands.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.client
import json
import os
import socket
import ssl
import statistics
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable
from urllib.parse import urlsplit


SENSITIVE_MARKERS = (
    "password",
    "passwd",
    "secret",
    "token",
    "apikey",
    "api_key",
    "userkey",
    "authkey",
    "private_key",
    "credential",
    "sid",
)


@dataclass(frozen=True)
class IniEntry:
    section: str
    key: str
    value: str

    @property
    def label(self) -> str:
        return f"{self.section}.{self.key}" if self.section else self.key


@dataclass(frozen=True)
class Response:
    status: int
    body: bytes
    elapsed_ms: float
    headers: tuple[tuple[str, str], ...]


def parse_ini(path: Path) -> list[IniEntry]:
    """Parse active, uncommented INI assignments without interpolation."""
    entries: list[IniEntry] = []
    section = ""
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        entries.append(IniEntry(section, key.strip().lower(), value.strip()))
    return entries


def is_sensitive(entry: IniEntry) -> bool:
    return entry.section == "code" or any(marker in entry.key for marker in SENSITIVE_MARKERS)


def configured_secrets(entries: Iterable[IniEntry]) -> dict[str, str]:
    return {
        entry.label: entry.value
        for entry in entries
        if is_sensitive(entry) and len(entry.value) >= 4
    }


def assignment_leaks(body: str, secrets: dict[str, str]) -> list[str]:
    """Return labels whose values occur in an active, unredacted assignment RHS."""
    leaks: set[str] = set()
    section = ""
    for raw_line in body.splitlines():
        line = raw_line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        entry = IniEntry(section, key.strip().lower(), value.strip())
        if is_sensitive(entry) and entry.value != "[redacted]":
            leaks.add(entry.label)
        for label, secret in secrets.items():
            if secret and secret in entry.value:
                leaks.add(label)
    return sorted(leaks)


def percentile(samples: list[float], fraction: float) -> float:
    ordered = sorted(samples)
    if not ordered:
        return 0.0
    index = min(len(ordered) - 1, int((len(ordered) - 1) * fraction + 0.999999))
    return ordered[index]


def create_ssl_context(insecure: bool, ca_file: Path | None) -> ssl.SSLContext:
    if insecure:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        return context
    if ca_file is not None:
        return ssl.create_default_context(cafile=str(ca_file))
    if sys.platform == "win32" and hasattr(ssl, "enum_certificates"):
        # Some bundled Windows Python/OpenSSL runtimes merge stale extra
        # certificates into create_default_context(). Load the Windows ROOT
        # store explicitly so path construction matches native applications.
        trusted_roots = "\n".join(
            ssl.DER_cert_to_PEM_cert(encoded)
            for encoded, encoding, _trust in ssl.enum_certificates("ROOT")
            if encoding == "x509_asn"
        )
        if not trusted_roots:
            raise RuntimeError("the Windows ROOT certificate store is empty")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.load_verify_locations(cadata=trusted_roots)
        return context
    return ssl.create_default_context()


class DeviceClient:
    def __init__(
        self,
        base_url: str,
        username: str,
        password: str,
        timeout: float,
        insecure: bool,
        ca_file: Path | None = None,
    ) -> None:
        parsed = urlsplit(base_url)
        if parsed.scheme not in {"http", "https"} or not parsed.hostname:
            raise ValueError("base URL must use http:// or https:// and include a host")
        self.scheme = parsed.scheme
        self.host = parsed.hostname
        self.port = parsed.port or (443 if parsed.scheme == "https" else 80)
        self.timeout = timeout
        self.origin = f"{self.scheme}://{parsed.netloc}"
        self.host_header = parsed.netloc
        self.authorization = "Basic " + base64.b64encode(
            f"{username}:{password}".encode("utf-8")
        ).decode("ascii")
        self.cookie: str | None = None
        self.context = create_ssl_context(insecure, ca_file)
        self.connection: http.client.HTTPConnection | None = None

    def _connect(self) -> http.client.HTTPConnection:
        if self.scheme == "https":
            return http.client.HTTPSConnection(
                self.host, self.port, timeout=self.timeout, context=self.context
            )
        return http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)

    def close(self) -> None:
        if self.connection is not None:
            self.connection.close()
            self.connection = None

    def request(
        self,
        path: str,
        *,
        authorize: bool = True,
        use_cookie: bool = True,
        retry: bool = True,
    ) -> Response:
        headers = {
            "Accept": "application/json, text/plain;q=0.9, */*;q=0.1",
            "Origin": self.origin,
            "User-Agent": "AD2IoT-HIL-Smoke/1",
        }
        if authorize:
            headers["Authorization"] = self.authorization
        if use_cookie and self.cookie:
            headers["Cookie"] = self.cookie
        if self.connection is None:
            self.connection = self._connect()
        started = time.perf_counter()
        try:
            self.connection.request("GET", path, headers=headers)
            raw = self.connection.getresponse()
            body = raw.read()
        except (BrokenPipeError, ConnectionError, http.client.HTTPException, OSError):
            self.close()
            if retry:
                return self.request(
                    path,
                    authorize=authorize,
                    use_cookie=use_cookie,
                    retry=False,
                )
            raise
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        response_headers = tuple(raw.getheaders())
        set_cookie = raw.getheader("Set-Cookie")
        if set_cookie:
            self.cookie = set_cookie.split(";", 1)[0]
        if raw.getheader("Connection", "").lower() == "close":
            self.close()
        return Response(raw.status, body, elapsed_ms, response_headers)


class WebSocketProbe:
    def __init__(self, client: DeviceClient) -> None:
        self.client = client
        self.sock: socket.socket | ssl.SSLSocket | None = None
        self.handshake_ms = 0.0

    @staticmethod
    def _recv_exact(sock: socket.socket, count: int) -> bytes:
        result = bytearray()
        while len(result) < count:
            chunk = sock.recv(count - len(result))
            if not chunk:
                raise ConnectionError("WebSocket closed while receiving a frame")
            result.extend(chunk)
        return bytes(result)

    @staticmethod
    def _frame(payload: bytes, opcode: int = 0x1) -> bytes:
        mask = os.urandom(4)
        length = len(payload)
        header = bytearray([0x80 | opcode])
        if length < 126:
            header.append(0x80 | length)
        elif length <= 0xFFFF:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))
        header.extend(mask)
        header.extend(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        return bytes(header)

    def _receive(self) -> tuple[int, bytes]:
        assert self.sock is not None
        first, second = self._recv_exact(self.sock, 2)
        opcode = first & 0x0F
        length = second & 0x7F
        masked = bool(second & 0x80)
        if length == 126:
            length = struct.unpack("!H", self._recv_exact(self.sock, 2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(self.sock, 8))[0]
        mask = self._recv_exact(self.sock, 4) if masked else b""
        payload = self._recv_exact(self.sock, length)
        if masked:
            payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        return opcode, payload

    def open_and_verify(self, partition: int, code: int) -> tuple[int, int]:
        started = time.perf_counter()
        raw_sock = socket.create_connection(
            (self.client.host, self.client.port), timeout=self.client.timeout
        )
        if self.client.scheme == "https":
            self.sock = self.client.context.wrap_socket(raw_sock, server_hostname=self.client.host)
        else:
            self.sock = raw_sock
        self.sock.settimeout(min(self.client.timeout, 5.0))
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            "GET /ad2ws HTTP/1.1\r\n"
            f"Host: {self.client.host_header}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"Origin: {self.client.origin}\r\n"
            f"Authorization: {self.client.authorization}\r\n\r\n"
        ).encode("ascii")
        self.sock.sendall(request)
        response = bytearray()
        while b"\r\n\r\n" not in response:
            if len(response) > 16 * 1024:
                raise RuntimeError("oversized WebSocket handshake response")
            chunk = self.sock.recv(1024)
            if not chunk:
                raise ConnectionError("WebSocket closed during handshake")
            response.extend(chunk)
        header_block, remainder = bytes(response).split(b"\r\n\r\n", 1)
        if remainder:
            raise RuntimeError("unexpected data appended to WebSocket handshake")
        lines = header_block.decode("iso-8859-1").split("\r\n")
        if " 101 " not in f" {lines[0]} ":
            raise RuntimeError(f"WebSocket handshake returned {lines[0]}")
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
        expected = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
        ).decode("ascii")
        if headers.get("sec-websocket-accept") != expected:
            raise RuntimeError("WebSocket accept hash did not validate")
        self.handshake_ms = (time.perf_counter() - started) * 1000.0

        for command in (
            f"!SYNC:{partition},{code}",
            "!PING:00000000",
            "!HISTORY:64",
        ):
            self.sock.sendall(self._frame(command.encode("utf-8")))

        json_messages = 0
        pongs = 0
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and (json_messages == 0 or pongs == 0):
            try:
                opcode, payload = self._receive()
            except socket.timeout:
                continue
            if opcode == 0x1:
                text = payload.decode("utf-8", errors="replace")
                if text.startswith("{"):
                    json_messages += 1
                elif text.startswith("!PONG:"):
                    pongs += 1
                elif text.startswith("!ERROR:"):
                    raise RuntimeError("WebSocket returned a protocol error")
            elif opcode == 0x8:
                close_code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 0
                close_reason = payload[2:].decode("utf-8", errors="replace")
                detail = f" code={close_code}" if close_code else ""
                if close_reason:
                    detail += f" reason={close_reason!r}"
                raise ConnectionError(f"WebSocket closed during verification{detail}")
            elif opcode == 0x9:
                self.sock.sendall(self._frame(payload, opcode=0xA))
        if json_messages == 0 or pongs == 0:
            raise RuntimeError("WebSocket did not return both state/history JSON and pong")
        return json_messages, pongs

    def close(self) -> None:
        if self.sock is None:
            return
        try:
            self.sock.settimeout(1.0)
            self.sock.sendall(self._frame(struct.pack("!H", 1000), opcode=0x8))
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline:
                try:
                    opcode, payload = self._receive()
                except (ConnectionError, socket.timeout):
                    break
                if opcode == 0x8:
                    break
                if opcode == 0x9:
                    self.sock.sendall(self._frame(payload, opcode=0xA))
        except OSError:
            pass
        finally:
            self.sock.close()
            self.sock = None


def decode_json(response: Response, label: str) -> dict:
    if response.status != 200:
        raise RuntimeError(f"{label} returned HTTP {response.status}")
    try:
        return json.loads(response.body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"{label} did not return valid JSON") from exc


def run(args: argparse.Namespace) -> None:
    entries = parse_ini(args.config)
    settings = {entry.label: entry.value for entry in entries}
    username = settings.get("webui.user", "")
    password = settings.get("webui.password", "")
    if not username or not password:
        raise RuntimeError("the local INI does not contain webui.user and webui.password")
    expected_version = args.expected_version or args.version_file.read_text(encoding="utf-8").strip()
    secrets = configured_secrets(entries)

    client = DeviceClient(
        args.base_url, username, password, args.timeout, args.insecure, args.ca_file
    )
    websocket = WebSocketProbe(client)
    config_times: list[float] = []
    small_times: list[float] = []
    try:
        first = decode_json(client.request("/api/system"), "system API")
        installed = first.get("firmware_version")
        if installed != expected_version:
            raise RuntimeError(
                f"installed firmware is {installed!r}; expected {expected_version!r}"
            )
        cookie_test = client.request("/api/system", authorize=False, use_cookie=True)
        if cookie_test.status != 200 or not client.cookie:
            raise RuntimeError("authenticated session cookie was not established or accepted")

        anonymous = DeviceClient(
            args.base_url, "", "", args.timeout, args.insecure, args.ca_file
        )
        try:
            denied = anonymous.request("/api/system", authorize=False, use_cookie=False)
        finally:
            anonymous.close()
        if denied.status != 401 or b"Authentication required" not in denied.body:
            raise RuntimeError("unauthenticated system API did not return a complete HTTP 401")

        json_messages, pongs = websocket.open_and_verify(args.partition, args.code)
        workload_start = decode_json(
            client.request("/api/system"), "post-WebSocket system API"
        )

        storage = first.get("storage", {})
        sources = ["active"]
        if storage.get("spiffs", {}).get("config", {}).get("present"):
            sources.append("spiffs")
        if storage.get("sd_card", {}).get("config", {}).get("present"):
            sources.append("sd")
        for _ in range(args.rounds):
            for source in sources:
                response = client.request(f"/api/config?source={source}")
                if response.status != 200:
                    raise RuntimeError(f"{source} config returned HTTP {response.status}")
                text = response.body.decode("utf-8", errors="replace")
                leaks = assignment_leaks(text, secrets)
                if leaks:
                    raise RuntimeError(
                        f"{source} config exposed sensitive assignment labels: {', '.join(leaks)}"
                    )
                config_times.append(response.elapsed_ms)

        paths = (
            "/api/system",
            f"/api/state?partition={args.partition}",
            "/api/history?limit=64",
            "/api/logs?limit=64",
            "/api/firmware",
        )
        for index in range(args.small_requests):
            response = client.request(paths[index % len(paths)])
            if response.status != 200:
                raise RuntimeError(
                    f"{paths[index % len(paths)]} returned HTTP {response.status}"
                )
            small_times.append(response.elapsed_ms)

        final = decode_json(client.request("/api/system"), "final system API")
        if final.get("firmware_version") != expected_version:
            raise RuntimeError("firmware identity changed during the workload")
        before_uptime = int(workload_start.get("uptime_ms", 0))
        after_uptime = int(final.get("uptime_ms", 0))
        if after_uptime <= before_uptime:
            raise RuntimeError("device uptime did not advance; a reboot may have occurred")
        before_heap = int(workload_start.get("memory", {}).get("free_heap_bytes", 0))
        after_heap = int(final.get("memory", {}).get("free_heap_bytes", 0))
        minimum_heap = int(final.get("memory", {}).get("minimum_free_heap_bytes", 0))
        heap_loss = before_heap - after_heap
        if heap_loss > args.max_heap_loss:
            raise RuntimeError(
                f"free heap fell by {heap_loss} bytes (limit {args.max_heap_loss})"
            )
        if minimum_heap < args.min_free_heap:
            raise RuntimeError(
                f"minimum free heap was {minimum_heap} bytes (limit {args.min_free_heap})"
            )

        print(f"PASS firmware={expected_version} sources={','.join(sources)}")
        print(
            "WSS "
            f"handshake_ms={websocket.handshake_ms:.1f} json={json_messages} pong={pongs}"
        )
        print(
            "Config "
            f"count={len(config_times)} median_ms={statistics.median(config_times):.1f} "
            f"p95_ms={percentile(config_times, 0.95):.1f} max_ms={max(config_times):.1f}"
        )
        print(
            "SmallAPI "
            f"count={len(small_times)} median_ms={statistics.median(small_times):.1f} "
            f"p95_ms={percentile(small_times, 0.95):.1f} max_ms={max(small_times):.1f}"
        )
        print(
            "Memory "
            f"before={before_heap} after={after_heap} delta={after_heap - before_heap} "
            f"minimum={minimum_heap}"
        )
    finally:
        websocket.close()
        client.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", required=True, help="device origin, for example https://device.example")
    parser.add_argument("--config", type=Path, default=Path("build/ad2iot.ini"))
    parser.add_argument("--version-file", type=Path, default=Path("version.txt"))
    parser.add_argument("--expected-version")
    parser.add_argument("--partition", type=int, default=1)
    parser.add_argument("--code", type=int, default=1)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--small-requests", type=int, default=25)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--max-heap-loss", type=int, default=4096)
    parser.add_argument("--min-free-heap", type=int, default=16_000)
    parser.add_argument("--ca-file", type=Path, help="PEM CA bundle for a private test CA")
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="disable certificate verification for an explicitly trusted test device",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.rounds < 1 or args.small_requests < 1:
        print("error: rounds and small-requests must both be positive", file=sys.stderr)
        return 2
    if args.insecure:
        print("WARNING: TLS certificate verification is disabled for this run", file=sys.stderr)
    try:
        run(args)
    except Exception as exc:  # concise CLI boundary; credentials are never included
        print(f"FAIL {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
