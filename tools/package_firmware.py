#!/usr/bin/env python3
"""Package a successful ESP-IDF build without connecting to any serial port.

Run with the ESP-IDF Python environment, which includes esptool.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def checked_relative_path(value: str) -> Path:
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError(f"Expected a build-relative firmware path: {value}")
    return path


def package(build_dir: Path, output_dir: Path) -> None:
    arguments = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    if arguments["extra_esptool_args"]["chip"] != "esp32s3":
        raise ValueError("This project packages ESP32-S3 firmware only")
    flash_files = arguments["flash_files"]
    files = [(offset, checked_relative_path(name)) for offset, name in flash_files.items()]
    files.sort(key=lambda entry: int(entry[0], 0))
    for _, relative_path in files:
        source = build_dir / relative_path
        if not source.is_file() or source.stat().st_size == 0:
            raise FileNotFoundError(f"Missing completed firmware: {source}")
    output_dir.mkdir(parents=True, exist_ok=True)
    packaged_paths: list[Path] = []
    for _, relative_path in files:
        target = output_dir / relative_path
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build_dir / relative_path, target)
        packaged_paths.append(relative_path)
    for name in ("flasher_args.json", "flash_args"):
        shutil.copy2(build_dir / name, output_dir / name)
        packaged_paths.append(Path(name))

    # All offsets and flash settings come from the successful IDF build.
    merge_args = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge_bin",
                  "-o", "merged-flash.bin", *arguments["write_flash_args"]]
    for offset, relative_path in files:
        merge_args.extend((offset, relative_path.as_posix()))
    subprocess.run(merge_args, cwd=output_dir, check=True)
    packaged_paths.append(Path("merged-flash.bin"))
    checksums = []
    for path in sorted(packaged_paths):
        digest = hashlib.sha256((output_dir / path).read_bytes()).hexdigest()
        checksums.append(f"{digest}  {path.as_posix()}")
    (output_dir / "SHA256SUMS.txt").write_text("\n".join(checksums) + "\n", encoding="utf-8")
    print(f"Packaged {len(packaged_paths)} files into {output_dir}")
    print("No serial device was opened. merged-flash.bin starts at 0x0 and overwrites NVS gaps.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    package(args.build_dir.resolve(), args.output_dir.resolve())


if __name__ == "__main__":
    main()
