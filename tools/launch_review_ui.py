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
import http.server
import json
import os
import socketserver
import threading
import webbrowser
from datetime import datetime
from urllib.parse import urlparse


ROOT = os.path.join(os.path.dirname(__file__), "review_ui")
SESSIONS_ROOT = os.path.join(os.path.dirname(__file__), "review_sessions")
HOST = "127.0.0.1"
PORT = 8765


def save_session(payload: dict) -> dict:
    os.makedirs(SESSIONS_ROOT, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    session_dir = os.path.join(SESSIONS_ROOT, f"session-{stamp}")
    os.makedirs(session_dir, exist_ok=True)

    source_image_b64 = payload.get("source_image_base64", "")
    source_image_filename = os.path.basename(str(payload.get("source_image_filename", "")).strip()) or "source-image"
    annotated_png_b64 = payload.get("annotated_png_base64", "")
    review = payload.get("review", {})

    if source_image_b64:
        with open(os.path.join(session_dir, source_image_filename), "wb") as f:
            f.write(base64.b64decode(source_image_b64))
    if annotated_png_b64:
        with open(os.path.join(session_dir, "annotated.png"), "wb") as f:
            f.write(base64.b64decode(annotated_png_b64))

    with open(os.path.join(session_dir, "review.json"), "w", encoding="utf-8") as f:
        json.dump(review, f, indent=2)

    notes = str(review.get("note", ""))
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
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
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
