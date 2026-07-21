#!/usr/bin/env python3
"""
verify.py — Full verification gate for the v1.1 firmware.

Runs, in order:
  1. host_check.py   — packet layout, validator logic, source invariants
  2. build_check.py  — compile both sketches via PlatformIO (pinned
                       espressif32@6.5.0 / core 2.0.14 / HX711 0.7.5)

Installs PlatformIO automatically if missing (pip --user).  Designed to
be handed to a local agent or CI runner as a single entry point:

  python Upgrades/tools/verify.py
  python Upgrades/tools/verify.py --board esp32dev
  python Upgrades/tools/verify.py --skip-build     # host checks only

Exit code 0 = all gates passed.
"""

import argparse
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent   # Upgrades/tools/


def _run(argv: list, label: str) -> int:
    print(f"\n=== {label} ===")
    rc = subprocess.run(argv).returncode
    print(f"=== {label}: {'PASS' if rc == 0 else f'FAIL (rc={rc})'} ===")
    return rc


def _have_platformio() -> bool:
    return bool(shutil.which("pio")) or \
        importlib.util.find_spec("platformio") is not None


def _install_platformio() -> int:
    print("\nPlatformIO not found — installing (pip --user) ...")
    return subprocess.run(
        [sys.executable, "-m", "pip", "install", "--user", "platformio"]
    ).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", default=None, metavar="BOARD_ID",
                        help="PlatformIO board override (default: lolin32_lite)")
    parser.add_argument("--skip-build", action="store_true",
                        help="run host checks only (no toolchain needed)")
    args = parser.parse_args()

    failed = []

    if _run([sys.executable, str(_HERE / "host_check.py")], "host_check"):
        failed.append("host_check")

    if not args.skip_build:
        if not _have_platformio() and _install_platformio() != 0:
            print("ERROR: PlatformIO install failed")
            failed.append("platformio-install")
        else:
            build = [sys.executable, str(_HERE / "build_check.py")]
            if args.board:
                build += ["--board", args.board]
            if _run(build, "build_check"):
                failed.append("build_check")

    print()
    if failed:
        print(f"VERIFY FAILED: {failed}")
        return 1
    print("VERIFY PASSED — all gates green.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
