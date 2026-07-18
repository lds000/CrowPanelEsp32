#!/usr/bin/env python3
"""
Launch the local browser review UI on localhost.

This version is intentionally file-based only:
  - import a screenshot/photo in the browser
  - annotate it
  - copy a chat-ready payload or save a local session

Usage:
  python tools/launch_review_ui.py
"""
from __future__ import annotations

import base64
import binascii
import http.server
import json
import os
import re
import socketserver
import struct
import threading
import uuid
import webbrowser
from datetime import datetime
from urllib.parse import urlparse


ROOT = os.path.join(os.path.dirname(__file__), "review_ui")
SESSIONS_ROOT = os.path.join(os.path.dirname(__file__), "review_sessions")
HOST = "127.0.0.1"
PORT = 8765
MAX_REQUEST_BYTES = int(os.environ.get("REVIEW_UI_MAX_REQUEST_BYTES", str(16 * 1024 * 1024)))
MAX_IMAGE_BYTES = int(os.environ.get("REVIEW_UI_MAX_IMAGE_BYTES", str(8 * 1024 * 1024)))
MAX_IMAGE_DIMENSION = 8192
ALLOWED_IMAGE_TYPES = {
    "image/bmp": ".bmp",
    "image/png": ".png",
    "image/jpeg": ".jpg",
    "image/gif": ".gif",
    "image/webp": ".webp",
}


def _sniff_image(data: bytes) -> tuple[str, int | None, int | None]:
    if (data.startswith(b"\x89PNG\r\n\x1a\n") and len(data) >= 45
            and data[12:16] == b"IHDR" and struct.unpack_from(">I", data, 8)[0] == 13
            and b"IEND" in data[-16:]):
        width, height = struct.unpack_from(">II", data, 16)
        return "image/png", width, height
    if (data.startswith(b"BM") and len(data) >= 54
            and struct.unpack_from("<I", data, 2)[0] == len(data)):
        width, height = struct.unpack_from("<ii", data, 18)
        return "image/bmp", width, abs(height)
    if data.startswith((b"GIF87a", b"GIF89a")) and len(data) >= 10:
        width, height = struct.unpack_from("<HH", data, 6)
        return "image/gif", width, height
    if data.startswith(b"RIFF") and data[8:12] == b"WEBP":
        if data[12:16] == b"VP8X" and len(data) >= 30:
            width = int.from_bytes(data[24:27], "little") + 1
            height = int.from_bytes(data[27:30], "little") + 1
            return "image/webp", width, height
        if data[12:16] == b"VP8 " and len(data) >= 30 and data[23:26] == b"\x9d\x01\x2a":
            width = struct.unpack_from("<H", data, 26)[0] & 0x3FFF
            height = struct.unpack_from("<H", data, 28)[0] & 0x3FFF
            return "image/webp", width, height
        if data[12:16] == b"VP8L" and len(data) >= 25 and data[20] == 0x2F:
            b0, b1, b2, b3 = data[21:25]
            width = 1 + b0 + ((b1 & 0x3F) << 8)
            height = 1 + (b1 >> 6) + (b2 << 2) + ((b3 & 0x0F) << 10)
            return "image/webp", width, height
        raise ValueError("unsupported or malformed WebP image")
    if data.startswith(b"\xff\xd8"):
        offset = 2
        while offset + 4 <= len(data):
            if data[offset] != 0xFF:
                offset += 1
                continue
            marker = data[offset + 1]
            offset += 2
            if marker in {0xD8, 0xD9}:
                continue
            if offset + 2 > len(data):
                break
            segment_len = struct.unpack_from(">H", data, offset)[0]
            if segment_len < 2 or offset + segment_len > len(data):
                break
            if marker in {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}:
                if segment_len < 7:
                    break
                height, width = struct.unpack_from(">HH", data, offset + 3)
                return "image/jpeg", width, height
            offset += segment_len
        raise ValueError("malformed JPEG image")
    raise ValueError("unsupported or invalid image signature")


def _decode_image(label: str, value: object, expected_mime: str | None = None) -> tuple[bytes, str]:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label} is missing")
    try:
        data = base64.b64decode(value, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise ValueError(f"{label} is not valid base64") from exc
    if not data or len(data) > MAX_IMAGE_BYTES:
        raise ValueError(f"{label} exceeds the allowed image size")
    mime, width, height = _sniff_image(data)
    if mime not in ALLOWED_IMAGE_TYPES:
        raise ValueError(f"{label} has an unsupported image type")
    if expected_mime and expected_mime != mime:
        raise ValueError(f"{label} MIME type does not match its contents")
    if width is None or height is None:
        raise ValueError(f"{label} dimensions could not be validated")
    if width <= 0 or height <= 0 or width > MAX_IMAGE_DIMENSION or height > MAX_IMAGE_DIMENSION:
        raise ValueError(f"{label} dimensions are invalid")
    return data, mime


def _safe_image_filename(value: object, mime: str) -> str:
    base = os.path.splitext(os.path.basename(str(value or "source-image")))[0]
    base = re.sub(r"[^A-Za-z0-9._-]+", "-", base).strip(".-")[:80] or "source-image"
    return base + ALLOWED_IMAGE_TYPES[mime]


def save_session(payload: dict) -> dict:
    if not isinstance(payload, dict):
        raise ValueError("request payload must be a JSON object")
    review = payload.get("review", {})
    if not isinstance(review, dict):
        raise ValueError("review must be a JSON object")
    source_declared_mime = review.get("screenshot", {}).get("mime_type") if isinstance(review.get("screenshot"), dict) else None
    if source_declared_mime is not None and source_declared_mime not in ALLOWED_IMAGE_TYPES:
        raise ValueError("declared source MIME type is unsupported")
    source_image, source_mime = _decode_image(
        "source_image_base64", payload.get("source_image_base64"), source_declared_mime
    )
    annotated_png, _ = _decode_image(
        "annotated_png_base64", payload.get("annotated_png_base64"), "image/png"
    )
    source_image_filename = _safe_image_filename(payload.get("source_image_filename"), source_mime)

    notes = str(review.get("note", ""))
    if len(notes) > 100_000:
        raise ValueError("review note is too large")
    annotations = review.get("annotations", [])
    if not isinstance(annotations, list) or len(annotations) > 10_000:
        raise ValueError("review annotations are invalid or excessive")

    os.makedirs(SESSIONS_ROOT, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    session_dir = os.path.join(SESSIONS_ROOT, f"session-{stamp}-{uuid.uuid4().hex[:8]}")
    os.makedirs(session_dir, exist_ok=False)

    with open(os.path.join(session_dir, source_image_filename), "xb") as f:
        f.write(source_image)
    with open(os.path.join(session_dir, "annotated.png"), "xb") as f:
        f.write(annotated_png)

    with open(os.path.join(session_dir, "review.json"), "w", encoding="utf-8") as f:
        json.dump(review, f, indent=2)

    with open(os.path.join(session_dir, "notes.txt"), "w", encoding="utf-8") as f:
        f.write(notes)

    return {
        "ok": True,
        "session_dir": session_dir,
        "files": sorted(os.listdir(session_dir)),
    }


def open_in_file_explorer(path: str) -> None:
    if os.name == "nt":
        os.startfile(path)  # type: ignore[attr-defined]
    else:
        webbrowser.open(f"file://{os.path.abspath(path)}")


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Content-Security-Policy", "default-src 'self'; img-src 'self' data: blob:; style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
        super().end_headers()

    def log_message(self, format: str, *args) -> None:
        super().log_message(format, *args)

    def _send_json(self, payload: dict, code: int = 200) -> None:
        data = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_text(self, text: str, code: int = 400) -> None:
        data = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        return super().do_GET()

    def do_POST(self):
        parsed = urlparse(self.path)
        try:
            content_length = self.headers.get("Content-Length")
            if content_length is None:
                self._send_text("Content-Length required", 411)
                return
            try:
                length = int(content_length)
            except ValueError:
                self._send_text("Invalid Content-Length", 400)
                return
            if length < 0 or length > MAX_REQUEST_BYTES:
                self._send_text("Request body too large", 413)
                return
            if self.headers.get("Content-Type", "").partition(";")[0].strip() != "application/json":
                self._send_text("Content-Type must be application/json", 415)
                return
            body = self.rfile.read(length)
            if len(body) != length:
                self._send_text("Incomplete request body", 400)
                return
            payload = json.loads(body.decode("utf-8")) if body else {}

            if parsed.path == "/api/save-session":
                self._send_json(save_session(payload))
                return

            if parsed.path == "/api/open-sessions-folder":
                os.makedirs(SESSIONS_ROOT, exist_ok=True)
                open_in_file_explorer(SESSIONS_ROOT)
                self._send_json({"ok": True, "path": SESSIONS_ROOT})
                return

            self._send_text("Not found", 404)
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            self._send_text(f"Invalid request: {exc}", 400)
        except OSError as exc:
            self._send_text(f"OS error: {exc}", 500)
        except Exception as exc:
            self._send_text(str(exc), 500)


def main() -> None:
    os.chdir(ROOT)

    class ThreadingServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
        allow_reuse_address = True
        daemon_threads = True

    chosen_port = None
    httpd = None
    for candidate in range(PORT, PORT + 10):
        try:
            httpd = ThreadingServer((HOST, candidate), Handler)
            chosen_port = candidate
            break
        except OSError:
            continue

    if httpd is None or chosen_port is None:
        raise RuntimeError("Could not bind any localhost port in range 8765-8774")

    with httpd:
        url = f"http://{HOST}:{chosen_port}/"
        print(f"Serving review UI at {url}")
        print("Backend API enabled: /api/save-session, /api/open-sessions-folder")

        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down.")


if __name__ == "__main__":
    main()
