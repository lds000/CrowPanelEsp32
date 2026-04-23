#!/usr/bin/env python3
"""
CrowPanel screenshot relay — hub Pi side.

Runs on the LawnBot hub Pi (192.168.68.88, Tailscale 100.116.147.6)
and serves a *cached* snapshot of the CrowPanel's on-device
screenshot endpoint (192.168.68.107:8080/capture.bmp).

Why cached: the panel's WiFi throughput for large payloads is ~3-7
KB/s, so a single 1.1 MB BMP takes minutes to download end-to-end.
Instead of making every client wait for that, a background worker on
the Pi keeps one warm copy refreshed on an interval (and on demand)
and serves it to clients in under a second.

Endpoints:
  GET /                        — HTML preview with auto-refresh
  GET /capture.bmp             — cached BMP passthrough
  GET /capture.png             — cached PNG (converted via Pillow)
  GET /controls.bmp            — wake controls, fetch fresh BMP, return it
  GET /controls.png            — wake controls, fetch fresh PNG, return it
  GET /wake-controls           — ask the panel to show its controls bar
  GET /refresh                 — trigger a background refresh (non-blocking)
  GET /refresh?wait=1          — trigger refresh and wait for it to finish
  GET /health                  — JSON status (age of cache, last fetch stats)
"""
from __future__ import annotations

import http.server
import io
import json
import os
import socketserver
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

PANEL_HOST: str = os.environ.get("PANEL_HOST", "192.168.68.107")
PANEL_PORT: int = int(os.environ.get("PANEL_PORT", "8080"))
BIND_HOST: str = os.environ.get("BIND_HOST", "0.0.0.0")
BIND_PORT: int = int(os.environ.get("BIND_PORT", "9108"))
FETCH_TIMEOUT: float = float(os.environ.get("FETCH_TIMEOUT", "420"))   # 7 min — slow WiFi
AUTO_REFRESH_SEC: float = float(os.environ.get("AUTO_REFRESH_SEC", "120"))  # background cadence

try:
    from PIL import Image  # type: ignore
    _HAVE_PIL = True
except Exception:
    _HAVE_PIL = False


# ─── Cache state (protected by _lock) ────────────────────────────────
_lock = threading.Lock()
_cache_bmp: bytes = b""
_cache_png: bytes = b""
_cache_fetched_at: float = 0.0         # epoch seconds of last SUCCESSFUL fetch
_last_attempt_at: float = 0.0
_last_attempt_ok: bool = False
_last_attempt_bytes: int = 0
_last_attempt_duration: float = 0.0
_last_attempt_error: str = ""

# A single worker does all panel fetches to avoid stampeding the panel.
_fetch_lock = threading.Lock()
_fetch_in_progress = threading.Event()


def _fetch_from_panel() -> tuple[bool, str]:
    """Fetch /capture.bmp from the panel and update the cache.

    Only one fetch runs at a time (enforced by _fetch_lock). Returns
    (success, message). Safe to call from the HTTP thread or the
    background refresher.
    """
    global _cache_bmp, _cache_png, _cache_fetched_at
    global _last_attempt_at, _last_attempt_ok, _last_attempt_bytes
    global _last_attempt_duration, _last_attempt_error

    acquired = _fetch_lock.acquire(blocking=False)
    if not acquired:
        # Another fetch is already running; just wait for it to finish
        # and then return the current cache state.
        _fetch_in_progress.wait(timeout=FETCH_TIMEOUT + 5)
        return (True, "coalesced with in-flight fetch")

    _fetch_in_progress.set()
    started = time.time()
    url = f"http://{PANEL_HOST}:{PANEL_PORT}/capture.bmp"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "crowpanel-relay/1.1"})
        with urllib.request.urlopen(req, timeout=FETCH_TIMEOUT) as resp:
            if resp.status != 200:
                raise RuntimeError(f"panel returned HTTP {resp.status}")
            body = resp.read()

        png_bytes = b""
        if _HAVE_PIL:
            try:
                img = Image.open(io.BytesIO(body))
                buf = io.BytesIO()
                img.save(buf, format="PNG", optimize=True)
                png_bytes = buf.getvalue()
            except Exception as e:
                sys.stderr.write(f"[fetch] BMP->PNG failed: {e}\n")

        with _lock:
            _cache_bmp = body
            _cache_png = png_bytes
            _cache_fetched_at = time.time()
            _last_attempt_at = _cache_fetched_at
            _last_attempt_ok = True
            _last_attempt_bytes = len(body)
            _last_attempt_duration = _cache_fetched_at - started
            _last_attempt_error = ""
        msg = f"ok: BMP {len(body)}B PNG {len(png_bytes)}B in {_last_attempt_duration:.1f}s"
        sys.stderr.write(f"[fetch] {msg}\n")
        return (True, msg)
    except Exception as e:
        dur = time.time() - started
        with _lock:
            _last_attempt_at = time.time()
            _last_attempt_ok = False
            _last_attempt_bytes = 0
            _last_attempt_duration = dur
            _last_attempt_error = str(e)
        msg = f"failed after {dur:.1f}s: {e}"
        sys.stderr.write(f"[fetch] {msg}\n")
        return (False, msg)
    finally:
        _fetch_in_progress.clear()
        _fetch_lock.release()


def _wake_panel_controls() -> tuple[bool, str]:
    """Ask the panel to show the transient controls bar."""
    url = f"http://{PANEL_HOST}:{PANEL_PORT}/wake-controls"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "crowpanel-relay/1.1"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read(200).decode("utf-8", errors="replace").strip()
            if resp.status != 200:
                raise RuntimeError(f"panel returned HTTP {resp.status}: {body}")
        return (True, body or "controls awake")
    except Exception as e:
        return (False, str(e))


def _background_refresher() -> None:
    """Periodically refresh the cache in the background. Silent warm-up."""
    time.sleep(2.0)
    while True:
        try:
            _fetch_from_panel()
        except Exception as e:
            sys.stderr.write(f"[bg] unexpected: {e}\n")
        time.sleep(AUTO_REFRESH_SEC)


# ─── HTTP server ─────────────────────────────────────────────────────

_INDEX_HTML = """<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<title>CrowPanel Screenshot Relay</title>
<style>
  body { background:#0f172a; color:#e2e8f0; font-family:system-ui,sans-serif; margin:0; padding:24px }
  h1 { color:#38bdf8; margin:0 0 4px }
  .sub { color:#64748b; margin-bottom:20px }
  .card { background:#1e293b; border:1px solid #334155; border-radius:8px; padding:16px; margin-bottom:16px; max-width:900px }
  a { color:#38bdf8 }
  img { max-width:100%; border:1px solid #334155; border-radius:4px; display:block; background:#000 }
  .row { display:flex; gap:12px; flex-wrap:wrap; margin-top:8px; align-items:center }
  button { background:#38bdf8; color:#0f172a; border:none; padding:8px 16px; border-radius:4px; cursor:pointer; font-weight:600 }
  button:hover { background:#0ea5e9 }
  button:disabled { background:#475569; cursor:not-allowed }
  code { background:#0f172a; padding:2px 6px; border-radius:3px; color:#94a3b8 }
  #status { font-family:ui-monospace,monospace; font-size:12px; color:#94a3b8 }
</style>
</head><body>
<h1>CrowPanel Screenshot Relay</h1>
<div class="sub">Cached relay — panel WiFi is slow (~5 min/BMP), so we keep a warm copy and refresh in the background.</div>

<div class="card">
  <strong>Controls</strong>
  <div class="row">
    <button onclick="refreshImage()">Reload cached image</button>
    <button id="pullBtn" onclick="pullFresh()">Pull fresh from panel (blocks until done)</button>
    <button id="controlsBtn" onclick="pullControls()">Show controls + capture</button>
  </div>
  <p id="status">loading...</p>
  <p><img id="shot" src="/capture.png" alt="screenshot"></p>
</div>

<div class="card">
  <strong>CLI</strong>
  <p><code>curl -o capture.png http://&lt;pi-ip&gt;:9108/capture.png</code></p>
  <p>Force a fresh pull and wait for it to finish (can take several minutes):<br>
     <code>curl -m 600 http://&lt;pi-ip&gt;:9108/refresh?wait=1</code></p>
</div>

<script>
async function updateStatus() {
  try {
    const r = await fetch("/health", { cache: "no-store" });
    const j = await r.json();
    const age = j.cache_age_sec;
    const ageStr = age === null ? "no cache yet" : age.toFixed(0) + "s ago";
    const last = j.last_attempt_ok ? ("ok " + j.last_attempt_bytes + "B in " + j.last_attempt_duration_sec.toFixed(1) + "s") : ("FAIL: " + (j.last_attempt_error || ""));
    document.getElementById("status").textContent =
      "cache: " + ageStr + "  |  last fetch: " + last +
      (j.fetch_in_progress ? "  |  refresh in progress..." : "");
  } catch(e) { /* ignore */ }
}
function refreshImage() {
  document.getElementById("shot").src = "/capture.png?t=" + Date.now();
}
async function pullFresh() {
  const btn = document.getElementById("pullBtn");
  btn.disabled = true;
  btn.textContent = "Pulling from panel... (may take minutes)";
  try {
    await fetch("/refresh?wait=1", { cache: "no-store" });
    refreshImage();
  } finally {
    btn.disabled = false;
    btn.textContent = "Pull fresh from panel (blocks until done)";
    updateStatus();
  }
}
async function pullControls() {
  const btn = document.getElementById("controlsBtn");
  btn.disabled = true;
  btn.textContent = "Waking controls...";
  try {
    document.getElementById("shot").src = "/controls.png?t=" + Date.now();
    await new Promise(resolve => setTimeout(resolve, 2500));
    updateStatus();
  } finally {
    btn.disabled = false;
    btn.textContent = "Show controls + capture";
  }
}
setInterval(updateStatus, 2000);
updateStatus();
</script>
</body></html>""".encode("utf-8")


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "CrowPanelRelay/1.1"

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write(f"[{datetime.now().strftime('%H:%M:%S')}] {self.address_string()} {fmt % args}\n")

    def _send(self, code: int, body: bytes, ctype: str = "text/plain; charset=utf-8") -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _parse_query(self) -> dict[str, str]:
        if "?" not in self.path:
            return {}
        _, qs = self.path.split("?", 1)
        out: dict[str, str] = {}
        for pair in qs.split("&"):
            if "=" in pair:
                k, v = pair.split("=", 1)
                out[k] = v
            elif pair:
                out[pair] = "1"
        return out

    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?", 1)[0]
        qs = self._parse_query()

        if path in ("/", "/index.html"):
            self._send(200, _INDEX_HTML, "text/html; charset=utf-8")
            return

        if path == "/capture.bmp":
            with _lock:
                body = _cache_bmp
            if not body:
                self._send(503, b"relay: no cached capture yet (still warming up)\n")
                return
            self._send(200, body, "image/bmp")
            return

        if path == "/capture.png":
            with _lock:
                body = _cache_png if _HAVE_PIL else _cache_bmp
                ctype = "image/png" if _HAVE_PIL else "image/bmp"
            if not body:
                self._send(503, b"relay: no cached capture yet (still warming up)\n")
                return
            self._send(200, body, ctype)
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
            with _lock:
                body = _cache_png if path == "/controls.png" and _HAVE_PIL else _cache_bmp
                ctype = "image/png" if path == "/controls.png" and _HAVE_PIL else "image/bmp"
            if not body:
                self._send(503, b"relay: controls capture produced no image\n")
                return
            self._send(200, body, ctype)
            return

        if path == "/refresh":
            if qs.get("wait") in ("1", "true", "yes"):
                ok, msg = _fetch_from_panel()
                self._send(200 if ok else 502, (msg + "\n").encode())
            else:
                threading.Thread(target=_fetch_from_panel, daemon=True).start()
                self._send(202, b"refresh triggered (async)\n")
            return

        if path == "/health":
            with _lock:
                now = time.time()
                age = (now - _cache_fetched_at) if _cache_fetched_at else None
                status: dict[str, object] = {
                    "relay": "ok",
                    "panel": f"http://{PANEL_HOST}:{PANEL_PORT}",
                    "pillow_available": _HAVE_PIL,
                    "cache_bytes_bmp": len(_cache_bmp),
                    "cache_bytes_png": len(_cache_png),
                    "cache_age_sec": age,
                    "cache_fetched_at": (datetime.fromtimestamp(_cache_fetched_at, tz=timezone.utc).isoformat()
                                         if _cache_fetched_at else None),
                    "last_attempt_at": (datetime.fromtimestamp(_last_attempt_at, tz=timezone.utc).isoformat()
                                        if _last_attempt_at else None),
                    "last_attempt_ok": _last_attempt_ok,
                    "last_attempt_bytes": _last_attempt_bytes,
                    "last_attempt_duration_sec": _last_attempt_duration,
                    "last_attempt_error": _last_attempt_error,
                    "fetch_in_progress": _fetch_in_progress.is_set(),
                    "auto_refresh_sec": AUTO_REFRESH_SEC,
                    "fetch_timeout_sec": FETCH_TIMEOUT,
                    "now": datetime.now(timezone.utc).isoformat(),
                }
            self._send(200, json.dumps(status, indent=2).encode(), "application/json")
            return

        self._send(404, b"not found\n")


class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main() -> int:
    server = ThreadedHTTPServer((BIND_HOST, BIND_PORT), Handler)
    print(f"[relay] panel         = http://{PANEL_HOST}:{PANEL_PORT}")
    print(f"[relay] listen        = http://{BIND_HOST}:{BIND_PORT}")
    print(f"[relay] pillow        = {_HAVE_PIL}")
    print(f"[relay] auto_refresh  = every {AUTO_REFRESH_SEC:.0f}s")
    print(f"[relay] fetch_timeout = {FETCH_TIMEOUT:.0f}s")

    bg = threading.Thread(target=_background_refresher, daemon=True)
    bg.start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[relay] shutting down")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
