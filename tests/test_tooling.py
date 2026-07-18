from __future__ import annotations

import base64
import importlib.util
import os
import struct
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


relay = load_module("crowpanel_relay", "tools/pi_screenshot_relay/relay.py")
review = load_module("crowpanel_review", "tools/launch_review_ui.py")
size_check = load_module("crowpanel_size", "tools/check_firmware_size.py")


def bmp(width: int = 800, height: int = 480) -> bytes:
    row = (width * 3 + 3) & ~3
    total = 54 + row * height
    data = bytearray(total)
    data[:2] = b"BM"
    struct.pack_into("<I", data, 2, total)
    struct.pack_into("<I", data, 10, 54)
    struct.pack_into("<IiiHHII", data, 14, 40, width, -height, 1, 24, 0, row * height)
    return bytes(data)


def tiny_png() -> bytes:
    ihdr = struct.pack(">I", 13) + b"IHDR" + struct.pack(">II", 1, 1) + b"\x08\x06\0\0\0" + b"\0\0\0\0"
    iend = struct.pack(">I", 0) + b"IEND" + b"\0\0\0\0"
    return b"\x89PNG\r\n\x1a\n" + ihdr + iend


class RelayTests(unittest.TestCase):
    def test_panel_actions_require_csrf_header(self):
        capture = relay._panel_request("/capture.bmp", 1.0)
        wake = relay._panel_request("/wake-controls", 1.0)
        self.assertIsNone(capture.get_header("X-crowpanel-action"))
        self.assertEqual(wake.get_header("X-crowpanel-action"), "1")

    def test_validates_expected_bmp(self):
        self.assertEqual(relay._validate_bmp(bmp()), (800, 480))
        with self.assertRaises(ValueError):
            relay._validate_bmp(b"not a bitmap")
        with self.assertRaises(ValueError):
            relay._validate_bmp(bmp(799, 480))

    def test_concurrent_callers_receive_real_fetch_result(self):
        started = threading.Event()
        release = threading.Event()
        calls = 0

        def slow_failure():
            nonlocal calls
            calls += 1
            started.set()
            release.wait(2)
            return False, "panel failed"

        with relay._fetch_condition:
            relay._fetch_active = False
            relay._fetch_generation = 0
            relay._fetch_result = (False, "not started")
        results = []
        with mock.patch.object(relay, "_perform_fetch", side_effect=slow_failure):
            first = threading.Thread(target=lambda: results.append(relay._fetch_from_panel()))
            second = threading.Thread(target=lambda: results.append(relay._fetch_from_panel()))
            first.start()
            self.assertTrue(started.wait(1))
            second.start()
            time.sleep(0.05)
            release.set()
            first.join(2)
            second.join(2)
        self.assertEqual(calls, 1)
        self.assertEqual(results, [(False, "panel failed"), (False, "panel failed")])

    def test_unknown_action_does_not_consume_rate_limit_entry(self):
        handler = object.__new__(relay.Handler)
        handler.client_address = ("192.0.2.10", 12345)
        sent = []
        handler._send = lambda code, body, ctype="text/plain": sent.append((code, body))
        with relay._action_lock:
            relay._last_action_by_client.clear()
        handler._action("/not-an-action", False)
        self.assertEqual(sent, [(404, b"not found\n")])
        self.assertEqual(relay._last_action_by_client, {})

    def test_rate_limit_map_has_a_hard_cap(self):
        handler = object.__new__(relay.Handler)
        handler.client_address = ("192.0.2.200", 12345)
        with relay._action_lock:
            relay._last_action_by_client.clear()
            for index in range(1024):
                relay._last_action_by_client[(f"198.51.{index // 256}.{index % 256}", "/refresh")] = 999.0
        with mock.patch.object(relay.time, "monotonic", return_value=1000.0):
            self.assertTrue(handler._rate_limit_action("/refresh"))
        self.assertEqual(len(relay._last_action_by_client), 1024)
        self.assertIn(("192.0.2.200", "/refresh"), relay._last_action_by_client)


class ReviewTests(unittest.TestCase):
    def test_rejects_mime_mismatch(self):
        value = base64.b64encode(tiny_png()).decode("ascii")
        with self.assertRaises(ValueError):
            review._decode_image("source", value, "image/jpeg")

    def test_unique_sanitized_session_names(self):
        encoded = base64.b64encode(tiny_png()).decode("ascii")
        payload = {
            "source_image_base64": encoded,
            "source_image_filename": "../../unsafe name.png",
            "annotated_png_base64": encoded,
            "review": {"note": "ok", "annotations": [], "screenshot": {"mime_type": "image/png"}},
        }
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(review, "SESSIONS_ROOT", tmp):
            one = review.save_session(payload)
            two = review.save_session(payload)
            self.assertNotEqual(one["session_dir"], two["session_dir"])
            self.assertIn("unsafe-name.png", one["files"])
            self.assertTrue(os.path.commonpath([tmp, one["session_dir"]]) == tmp)


class SizeTests(unittest.TestCase):
    def test_parse_partition_sizes(self):
        self.assertEqual(size_check.parse_size("0x1000"), 4096)
        self.assertEqual(size_check.parse_size("3M"), 3 * 1024 * 1024)
        self.assertEqual(size_check.parse_size("64K"), 64 * 1024)


if __name__ == "__main__":
    unittest.main()
