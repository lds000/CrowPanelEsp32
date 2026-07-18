#!/usr/bin/env python3
"""Authenticated, bounded screenshot cache for the CrowPanel.

The panel is deliberately fetched by one worker at a time. Concurrent callers
join that fetch and receive its real result rather than starting a stampede.
Mutating operations are POST-only unless explicitly enabled for migration.
"""
from __future__ import annotations

import base64
import hmac
import http.server
import io
import json
import os
import socketserver
import struct
import sys
import threading
import time
import urllib.request
from datetime import datetime, timezone


def _env_bool(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _env_int(name: str, default: int, minimum: int, maximum: int) -> int:
    try:
        value = int(os.environ.get(name, str(default)))
    except ValueError as exc:
        raise SystemExit(f"{name} must be an integer") from exc
    if not minimum <= value <= maximum:
        raise SystemExit(f"{name} must be between {minimum} and {maximum}")
    return value


def _env_float(name: str, default: float, minimum: float, maximum: float) -> float:
    try:
        value = float(os.environ.get(name, str(default)))
    except ValueError as exc:
        raise SystemExit(f"{name} must be a number") from exc
    if not minimum <= value <= maximum:
        raise SystemExit(f"{name} must be between {minimum} and {maximum}")
    return value


PANEL_HOST = os.environ.get("PANEL_HOST", "192.168.68.107")
PANEL_PORT = _env_int("PANEL_PORT", 8080, 1, 65535)
PANEL_AUTH_USER = os.environ.get("PANEL_AUTH_USER", "crowpanel")
PANEL_AUTH_TOKEN = os.environ.get("PANEL_AUTH_TOKEN", "")

# Safe by default. Non-loopback binds require a token or an explicit opt-out.
BIND_HOST = os.environ.get("BIND_HOST", "127.0.0.1")
BIND_PORT = _env_int("BIND_PORT", 9108, 1, 65535)
RELAY_TOKEN = os.environ.get("RELAY_TOKEN", "")
ALLOW_UNAUTHENTICATED = _env_bool("ALLOW_UNAUTHENTICATED")
ALLOW_LEGACY_GET_ACTIONS = _env_bool("ALLOW_LEGACY_GET_ACTIONS")
CORS_ALLOW_ORIGIN = os.environ.get("CORS_ALLOW_ORIGIN", "").strip()

FETCH_TIMEOUT = _env_float("FETCH_TIMEOUT", 420.0, 1.0, 900.0)
AUTO_REFRESH_SEC = _env_float("AUTO_REFRESH_SEC", 120.0, 10.0, 86400.0)
ACTION_MIN_INTERVAL_SEC = _env_float("ACTION_MIN_INTERVAL_SEC", 2.0, 0.0, 3600.0)
MAX_CAPTURE_BYTES = _env_int("MAX_CAPTURE_BYTES", 2_000_000, 1024, 20_000_000)
MAX_SERVER_THREADS = _env_int("MAX_SERVER_THREADS", 8, 1, 64)
MAX_ACTION_BODY_BYTES = _env_int("MAX_ACTION_BODY_BYTES", 1024, 0, 65536)
REQUEST_TIMEOUT_SEC = _env_float("REQUEST_TIMEOUT_SEC", 15.0, 1.0, 300.0)
EXPECTED_WIDTH = _env_int("EXPECTED_WIDTH", 800, 1, 8192)
EXPECTED_HEIGHT = _env_int("EXPECTED_HEIGHT", 480, 1, 8192)

try:
    from PIL import Image  # type: ignore
    _HAVE_PIL = True
except Exception:
    _HAVE_PIL = False


_lock = threading.Lock()
_cache_bmp = b""
_cache_png = b""
_cache_fetched_at = 0.0
_last_attempt_at = 0.0
_last_attempt_ok = False
_last_attempt_bytes = 0
_last_attempt_duration = 0.0
_last_attempt_error = ""

# A condition carries both completion and the exact result for joined callers.
_fetch_condition = threading.Condition()
_fetch_active = False
_fetch_generation = 0
_fetch_result: tuple[bool, str] = (False, "no fetch attempted")

_action_lock = threading.Lock()
_last_action_by_client: dict[tuple[str, str], float] = {}
_ACTION_PATHS = {"/refresh", "/wake-controls", "/controls.bmp", "/controls.png"}


def _panel_request(path: str, timeout: float) -> urllib.request.Request:
    headers = {"User-Agent": "crowpanel-relay/2.0"}
    if PANEL_AUTH_TOKEN:
        raw = f"{PANEL_AUTH_USER}:{PANEL_AUTH_TOKEN}".encode("utf-8")
        headers["Authorization"] = "Basic " + base64.b64encode(raw).decode("ascii")
    if path != "/capture.bmp":
        headers["X-CrowPanel-Action"] = "1"
    return urllib.request.Request(
        f"http://{PANEL_HOST}:{PANEL_PORT}{path}",
        headers=headers,
        method="GET" if path == "/capture.bmp" else "POST",
    )


def _validate_bmp(body: bytes) -> tuple[int, int]:
    """Validate the fixed-format capture before publishing it to clients."""
    if len(body) < 54 or len(body) > MAX_CAPTURE_BYTES:
        raise ValueError(f"BMP size {len(body)} is outside allowed range")
    if body[:2] != b"BM":
        raise ValueError("capture is not a BMP")
    declared_size = struct.unpack_from("<I", body, 2)[0]
    pixel_offset = struct.unpack_from("<I", body, 10)[0]
    dib_size = struct.unpack_from("<I", body, 14)[0]
    width, height = struct.unpack_from("<ii", body, 18)
    planes, bpp = struct.unpack_from("<HH", body, 26)
    compression = struct.unpack_from("<I", body, 30)[0]
    abs_height = abs(height)
    if declared_size != len(body) or dib_size < 40 or pixel_offset < 54:
        raise ValueError("BMP header size fields are inconsistent")
    if width != EXPECTED_WIDTH or abs_height != EXPECTED_HEIGHT:
        raise ValueError(f"unexpected BMP dimensions {width}x{abs_height}")
    if planes != 1 or bpp != 24 or compression != 0:
        raise ValueError("expected an uncompressed 24-bit BMP")
    row_size = (width * 3 + 3) & ~3
    if pixel_offset + row_size * abs_height != len(body):
        raise ValueError("BMP pixel data length is inconsistent")
    return width, abs_height


def _read_bounded_response(resp) -> bytes:
    length = resp.headers.get("Content-Length")
    if length:
        try:
            if int(length) > MAX_CAPTURE_BYTES:
                raise ValueError("panel capture exceeds MAX_CAPTURE_BYTES")
        except ValueError as exc:
            raise ValueError("invalid or excessive Content-Length") from exc
    body = resp.read(MAX_CAPTURE_BYTES + 1)
    if len(body) > MAX_CAPTURE_BYTES:
        raise ValueError("panel capture exceeds MAX_CAPTURE_BYTES")
    return body


def _perform_fetch() -> tuple[bool, str]:
    global _cache_bmp, _cache_png, _cache_fetched_at
    global _last_attempt_at, _last_attempt_ok, _last_attempt_bytes
    global _last_attempt_duration, _last_attempt_error

    started = time.time()
    try:
        req = _panel_request("/capture.bmp", FETCH_TIMEOUT)
        with urllib.request.urlopen(req, timeout=FETCH_TIMEOUT) as resp:
            if resp.status != 200:
                raise RuntimeError(f"panel returned HTTP {resp.status}")
            body = _read_bounded_response(resp)
        width, height = _validate_bmp(body)

        png_bytes = b""
        if _HAVE_PIL:
            try:
                with Image.open(io.BytesIO(body)) as img:
                    if img.size != (width, height):
                        raise ValueError("decoder dimensions differ from BMP header")
                    buf = io.BytesIO()
                    img.save(buf, format="PNG", optimize=True)
                    png_bytes = buf.getvalue()
            except Exception as exc:
                sys.stderr.write(f"[fetch] BMP-to-PNG failed: {exc}\n")

        finished = time.time()
        with _lock:
            _cache_bmp = body
            _cache_png = png_bytes
            _cache_fetched_at = finished
            _last_attempt_at = finished
            _last_attempt_ok = True
            _last_attempt_bytes = len(body)
            _last_attempt_duration = finished - started
            _last_attempt_error = ""
        msg = f"ok: BMP {len(body)}B PNG {len(png_bytes)}B in {finished - started:.1f}s"
        sys.stderr.write(f"[fetch] {msg}\n")
        return True, msg
    except Exception as exc:
        duration = time.time() - started
        with _lock:
            _last_attempt_at = time.time()
            _last_attempt_ok = False
            _last_attempt_bytes = 0
            _last_attempt_duration = duration
            _last_attempt_error = str(exc)
        msg = f"failed after {duration:.1f}s: {exc}"
        sys.stderr.write(f"[fetch] {msg}\n")
        return False, msg


def _fetch_from_panel() -> tuple[bool, str]:
    """Run one fetch, or join an in-flight fetch and return its real result."""
    global _fetch_active, _fetch_generation, _fetch_result

    with _fetch_condition:
        if _fetch_active:
            generation = _fetch_generation
            completed = _fetch_condition.wait_for(
                lambda: _fetch_generation != generation,
                timeout=FETCH_TIMEOUT + 5,
            )
            if not completed:
                return False, "timed out waiting for in-flight fetch"
            return _fetch_result
        _fetch_active = True

    result = _perform_fetch()
    with _fetch_condition:
        _fetch_result = result
        _fetch_generation += 1
        _fetch_active = False
        _fetch_condition.notify_all()
    return result


def _fetch_is_active() -> bool:
    with _fetch_condition:
        return _fetch_active


def _wake_panel_controls() -> tuple[bool, str]:
    try:
        req = _panel_request("/wake-controls", 10.0)
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read(201).decode("utf-8", errors="replace").strip()
            if len(body) > 200:
                raise RuntimeError("panel response was too large")
            if resp.status != 200:
                raise RuntimeError(f"panel returned HTTP {resp.status}: {body}")
        return True, body or "controls awake"
    except Exception as exc:
        return False, str(exc)


def _background_refresher() -> None:
    time.sleep(2.0)
    while True:
        _fetch_from_panel()
        time.sleep(AUTO_REFRESH_SEC)


_INDEX_HTML = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CrowPanel Screenshot Relay</title><style>
body{background:#0f172a;color:#e2e8f0;font-family:system-ui,sans-serif;margin:0;padding:24px}
h1{color:#38bdf8}.card{background:#1e293b;border:1px solid #334155;border-radius:8px;padding:16px;max-width:900px}
img{max-width:100%;border:1px solid #334155;background:#000}button{margin:4px;padding:8px 14px}code{color:#94a3b8}
</style></head><body><h1>CrowPanel Screenshot Relay</h1><div class="card">
<p>The panel is slow, so this page serves a validated cache instead of organizing a tiny denial-of-service festival.</p>
<button onclick="reloadCached()">Reload cache</button>
<button id="fresh" onclick="action('/refresh?wait=1')">Pull fresh</button>
<button id="controls" onclick="controls()">Show controls + capture</button>
<p id="status">Loading...</p><img id="shot" src="/capture.png" alt="CrowPanel screenshot"></div>
<script>
const shot=document.getElementById('shot');
function reloadCached(){shot.src='/capture.png?t='+Date.now()}
const actionHeaders={'X-CrowPanel-Action':'1'};
async function action(path){const r=await fetch(path,{method:'POST',headers:actionHeaders,cache:'no-store'});if(!r.ok)throw new Error(await r.text());reloadCached();await status()}
async function controls(){const r=await fetch('/controls.png',{method:'POST',headers:actionHeaders,cache:'no-store'});if(!r.ok)throw new Error(await r.text());const u=URL.createObjectURL(await r.blob());shot.onload=()=>URL.revokeObjectURL(u);shot.src=u;await status()}
async function status(){try{const r=await fetch('/health',{cache:'no-store'});const j=await r.json();document.getElementById('status').textContent=JSON.stringify(j)}catch(e){document.getElementById('status').textContent=e.message}}
setInterval(status,5000);status();
</script></body></html>""".encode("utf-8")


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "CrowPanelRelay/2.0"

    def setup(self) -> None:
        super().setup()
        self.connection.settimeout(REQUEST_TIMEOUT_SEC)

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write(f"[{datetime.now().strftime('%H:%M:%S')}] {self.client_address[0]} {fmt % args}\n")

    def _authorized(self) -> bool:
        if not RELAY_TOKEN:
            return True
        value = self.headers.get("Authorization", "")
        supplied = ""
        if value.startswith("Bearer "):
            supplied = value[7:]
        elif value.startswith("Basic "):
            try:
                decoded = base64.b64decode(value[6:], validate=True).decode("utf-8")
                supplied = decoded.partition(":")[2]
            except (ValueError, UnicodeDecodeError):
                supplied = ""
        if supplied and hmac.compare_digest(supplied, RELAY_TOKEN):
            return True
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="CrowPanel relay"')
        self.send_header("Content-Length", "0")
        self.end_headers()
        return False

    def _action_request_allowed(self) -> bool:
        """Block form-based CSRF even when a browser cached Basic auth."""
        if not hmac.compare_digest(self.headers.get("X-CrowPanel-Action", ""), "1"):
            self._send(403, b"missing action confirmation header\n")
            return False
        if self.headers.get("Sec-Fetch-Site", "").lower() == "cross-site":
            self._send(403, b"cross-site action rejected\n")
            return False
        return True

    def _send(self, code: int, body: bytes, ctype: str = "text/plain; charset=utf-8") -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        origin = self.headers.get("Origin", "")
        if CORS_ALLOW_ORIGIN and origin == CORS_ALLOW_ORIGIN:
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def _rate_limit_action(self, path: str) -> bool:
        if ACTION_MIN_INTERVAL_SEC <= 0:
            return True
        key = (self.client_address[0], path)
        now = time.monotonic()
        with _action_lock:
            last = _last_action_by_client.get(key, 0.0)
            if now - last < ACTION_MIN_INTERVAL_SEC:
                retry = max(1, int(ACTION_MIN_INTERVAL_SEC - (now - last) + 0.999))
                self.send_response(429)
                self.send_header("Retry-After", str(retry))
                self.send_header("Content-Length", "0")
                self.end_headers()
                return False
            # Keep malicious rotating clients from growing this forever. Paths
            # are validated before this function, and the hard cap applies even
            # when every entry is recent.
            if key not in _last_action_by_client and len(_last_action_by_client) >= 1024:
                cutoff = now - max(ACTION_MIN_INTERVAL_SEC * 2, 60)
                stale = [k for k, value in _last_action_by_client.items() if value < cutoff]
                for old_key in stale:
                    _last_action_by_client.pop(old_key, None)
                while len(_last_action_by_client) >= 1024:
                    oldest = min(_last_action_by_client, key=_last_action_by_client.get)
                    _last_action_by_client.pop(oldest, None)
            _last_action_by_client[key] = now
        return True

    def _health(self) -> None:
        with _lock:
            age = time.time() - _cache_fetched_at if _cache_fetched_at else None
            status = {
                "relay": "ok",
                "pillow_available": _HAVE_PIL,
                "cache_bytes_bmp": len(_cache_bmp),
                "cache_bytes_png": len(_cache_png),
                "cache_age_sec": age,
                "cache_fetched_at": datetime.fromtimestamp(_cache_fetched_at, tz=timezone.utc).isoformat() if _cache_fetched_at else None,
                "last_attempt_at": datetime.fromtimestamp(_last_attempt_at, tz=timezone.utc).isoformat() if _last_attempt_at else None,
                "last_attempt_ok": _last_attempt_ok,
                "last_attempt_bytes": _last_attempt_bytes,
                "last_attempt_duration_sec": _last_attempt_duration,
                "last_attempt_error": _last_attempt_error,
                "fetch_in_progress": _fetch_is_active(),
                "auto_refresh_sec": AUTO_REFRESH_SEC,
                "now": datetime.now(timezone.utc).isoformat(),
            }
        self._send(200, json.dumps(status, indent=2).encode(), "application/json")

    def _cached(self, png: bool) -> None:
        with _lock:
            use_png = png and _HAVE_PIL and bool(_cache_png)
            body = _cache_png if use_png else _cache_bmp
        if not body:
            self._send(503, b"relay: no cached capture yet\n")
            return
        self._send(200, body, "image/png" if use_png else "image/bmp")

    def _action(self, path: str, wait: bool) -> None:
        if path not in _ACTION_PATHS:
            self._send(404, b"not found\n")
            return
        if not self._rate_limit_action(path):
            return
        if path == "/refresh":
            if wait:
                ok, msg = _fetch_from_panel()
                self._send(200 if ok else 502, (msg + "\n").encode())
            else:
                threading.Thread(target=_fetch_from_panel, daemon=True).start()
                self._send(202, b"refresh triggered\n")
            return
        if path in ("/wake-controls", "/controls.bmp", "/controls.png"):
            ok, msg = _wake_panel_controls()
            if not ok:
                self._send(502, ("panel wake failed: " + msg + "\n").encode())
                return
            if path == "/wake-controls":
                self._send(200, (msg + "\n").encode())
                return
            time.sleep(0.75)
            ok, msg = _fetch_from_panel()
            if not ok:
                self._send(502, ("panel fetch failed: " + msg + "\n").encode())
                return
            self._cached(path.endswith(".png"))
            return

    def do_GET(self) -> None:  # noqa: N802
        if not self._authorized():
            return
        path, _, query = self.path.partition("?")
        if path in ("/", "/index.html"):
            self._send(200, _INDEX_HTML, "text/html; charset=utf-8")
        elif path == "/capture.bmp":
            self._cached(False)
        elif path == "/capture.png":
            self._cached(True)
        elif path == "/health":
            self._health()
        elif path in ("/refresh", "/wake-controls", "/controls.bmp", "/controls.png"):
            if not ALLOW_LEGACY_GET_ACTIONS:
                self.send_response(405)
                self.send_header("Allow", "POST")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self._action(path, "wait=1" in query)
        else:
            self._send(404, b"not found\n")

    def do_POST(self) -> None:  # noqa: N802
        if not self._authorized():
            return
        if not self._action_request_allowed():
            return
        path, _, query = self.path.partition("?")
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._send(400, b"invalid Content-Length\n")
            return
        if length < 0 or length > MAX_ACTION_BODY_BYTES:
            self._send(413, b"request body too large\n")
            return
        if length:
            try:
                body = self.rfile.read(length)
            except (TimeoutError, OSError):
                self._send(408, b"request body timed out\n")
                return
            if len(body) != length:
                self._send(400, b"incomplete request body\n")
                return
        self._action(path, "wait=1" in query)


class BoundedThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 16

    def __init__(self, server_address, handler):
        self._slots = threading.BoundedSemaphore(MAX_SERVER_THREADS)
        super().__init__(server_address, handler)

    def process_request(self, request, client_address):
        if not self._slots.acquire(blocking=False):
            try:
                request.sendall(b"HTTP/1.1 503 Busy\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")
            finally:
                self.shutdown_request(request)
            return
        super().process_request(request, client_address)

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._slots.release()


def _is_loopback(host: str) -> bool:
    return host in {"127.0.0.1", "::1", "localhost"}


def main() -> int:
    if not _is_loopback(BIND_HOST) and not RELAY_TOKEN and not ALLOW_UNAUTHENTICATED:
        raise SystemExit("Refusing non-loopback bind without RELAY_TOKEN; set ALLOW_UNAUTHENTICATED=1 only on a trusted isolated network")
    server = BoundedThreadingHTTPServer((BIND_HOST, BIND_PORT), Handler)
    print(f"[relay] panel = http://{PANEL_HOST}:{PANEL_PORT}")
    print(f"[relay] listen = http://{BIND_HOST}:{BIND_PORT}")
    print(f"[relay] auth = {'enabled' if RELAY_TOKEN else 'loopback/explicit opt-out'}")
    print(f"[relay] limits = {MAX_SERVER_THREADS} clients, {MAX_CAPTURE_BYTES} capture bytes")
    threading.Thread(target=_background_refresher, daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[relay] shutting down")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
