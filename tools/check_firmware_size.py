#!/usr/bin/env python3
"""Fail CI when a firmware binary exceeds its OTA application budget."""
from __future__ import annotations

import argparse
import csv
import os
import sys


def parse_size(value: str) -> int:
    value = value.strip().lower()
    multipliers = {"k": 1024, "m": 1024 * 1024}
    if value.endswith(tuple(multipliers)):
        return int(value[:-1], 0) * multipliers[value[-1]]
    return int(value, 0)


def app_partition_size(path: str) -> int:
    with open(path, encoding="utf-8") as handle:
        rows = (line for line in handle if line.strip() and not line.lstrip().startswith("#"))
        for row in csv.reader(rows):
            if len(row) >= 5 and row[0].strip() == "app0":
                return parse_size(row[4])
    raise ValueError("partitions file has no app0 entry")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--environment", default="crowpanel-7inch-demo")
    parser.add_argument("--build-dir", default=os.path.join(".pio", "build"))
    parser.add_argument("--partitions", default="partitions.csv")
    parser.add_argument("--max-percent", type=float, default=float(os.environ.get("MAX_FIRMWARE_PERCENT", "90")))
    args = parser.parse_args()
    binary = os.path.join(args.build_dir, args.environment, "firmware.bin")
    if not os.path.isfile(binary):
        parser.error(f"firmware binary not found: {binary}")
    used = os.path.getsize(binary)
    capacity = app_partition_size(args.partitions)
    percent = used * 100.0 / capacity
    print(f"firmware {used:,} / {capacity:,} bytes ({percent:.2f}%, limit {args.max_percent:.2f}%)")
    if percent > args.max_percent:
        print("Firmware exceeds the OTA partition budget", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
