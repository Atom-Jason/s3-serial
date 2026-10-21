#!/usr/bin/env python3
"""Verify a release package offline, without opening a serial port."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def verify(folder: Path) -> None:
    folder = folder.resolve()
    checks = 0
    for line in (folder / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines():
        digest, name = line.split("  ", 1)
        target = (folder / name).resolve()
        if not target.is_relative_to(folder):
            raise ValueError("Checksum path leaves firmware directory")
        if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            raise ValueError(f"SHA256 mismatch: {name}")
        checks += 1
    args = json.loads((folder / "flasher_args.json").read_text(encoding="utf-8"))
    if args["extra_esptool_args"]["chip"] != "esp32s3":
        raise ValueError("Expected ESP32-S3 flash arguments")
    merged = (folder / "merged-flash.bin").read_bytes()
    for address, name in args["flash_files"].items():
        target = (folder / name).resolve()
        if not target.is_relative_to(folder):
            raise ValueError("Flash path leaves firmware directory")
        raw = target.read_bytes()
        offset = int(address, 0)
        if offset < 0 or not raw or merged[offset:offset + len(raw)] != raw:
            raise ValueError(f"Merged image does not match {name} at {address}")
    if len(merged) > 4 * 1024 * 1024:
        raise ValueError("Merged image exceeds the configured 4 MB address space")
    print(f"Verified {checks} SHA256 checksums, ESP32-S3 target and all merged image offsets.")
    print("No serial device was opened. Hardware UART/RS-485 tests are separate.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware-dir", type=Path, default=Path(__file__).resolve().parent.parent / "firmware")
    args = parser.parse_args()
    verify(args.firmware_dir)


if __name__ == "__main__":
    main()
