#!/usr/bin/env python3
"""Small guard against committing the project's common secret shapes."""
from __future__ import annotations

import os
import re
import subprocess
import sys


RULES = (
    (re.compile(r'^\s*#define\s+WIFI_PASSWORD\s+"([^"\r\n]*)"', re.MULTILINE), "literal Wi-Fi password"),
    (re.compile(r'^\s*#define\s+(?:LAWNBOT_API_BEARER_TOKEN|OTA_PASSWORD|SCREENSHOT_HTTP_AUTH_TOKEN)\s+"([^"\r\n]*)"', re.MULTILINE), "literal device/API credential"),
    (re.compile(r'--auth=([^\s]+)'), "literal PlatformIO OTA credential"),
    (re.compile(r'curl(?:\.exe)?\s+[^\r\n]*\s-u\s+[^\s:$]+:([^\s<$]+)', re.IGNORECASE), "literal curl basic-auth credential"),
)


def _is_placeholder(value: str) -> bool:
    normalized = value.strip('"\'').lower()
    return (
        not normalized
        or normalized.startswith(("your-", "replace-", "change-me", "${"))
        or normalized in {"password", "token", "example"}
    )


def main() -> int:
    files = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"], text=True
    ).splitlines()
    errors: list[str] = []
    for path in files:
        normalized = path.replace("\\", "/")
        if normalized == "tools/check_tracked_secrets.py":
            continue
        if normalized.endswith("config_private.h"):
            errors.append(f"{path}: private configuration must not be tracked")
            continue
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except (OSError, UnicodeError):
            continue
        for pattern, label in RULES:
            match = pattern.search(text)
            if match and not _is_placeholder(match.group(1)):
                errors.append(f"{path}: possible {label}")
    if errors:
        print("Tracked-secret check failed:", file=sys.stderr)
        print("\n".join(f"- {item}" for item in errors), file=sys.stderr)
        return 1
    print(f"Tracked-secret check passed ({len(files)} files scanned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
