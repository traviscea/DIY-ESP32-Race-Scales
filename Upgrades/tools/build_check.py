#!/usr/bin/env python3
"""
build_check.py — Compile both v1.1 firmwares via PlatformIO.

Creates a throwaway PlatformIO project for each sketch, compiles it,
and reports pass/fail.  No files are modified in the repository.

Usage (from Upgrades/ or anywhere):
  python tools/build_check.py
  python tools/build_check.py --board esp32dev
  python tools/build_check.py --board esp32doit-devkit-v1

Pinned versions
  Platform : espressif32@6.5.0  (Arduino ESP32 core 2.0.14)
  HX711    : bogde/HX711@0.7.5
  Default board: lolin32_lite  (compatible with the ESP32 Lite Rev1,
                                4 MB flash, LiPo-interface board in the BOM)

Board override: pass --board <pio-board-id> to target a specific variant.

Requires: PlatformIO Core CLI (pio) on PATH.
  Install: https://docs.platformio.org/en/latest/core/installation/
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# ── Constants ─────────────────────────────────────────────────────────────────

_HERE    = Path(__file__).resolve().parent   # Upgrades/tools/
UPGRADES = _HERE.parent                      # Upgrades/


def _pio_cmd() -> list:
    """Locate PlatformIO: prefer 'pio' on PATH, fall back to the module
    runner ('python -m platformio'), which covers pip --user installs on
    Windows where the Scripts dir is not on PATH."""
    if shutil.which("pio"):
        return ["pio"]
    return [sys.executable, "-m", "platformio"]

PLATFORM      = "espressif32@6.5.0"
HX711_LIB     = "bogde/HX711@0.7.5"
DEFAULT_BOARD = "lolin32_lite"

SKETCHES = [
    ("child",  "diy_race_scale_children_esp32.ino"),
    ("parent", "diy_race_scale_parent_esp32.ino"),
]

PROTOCOL_HEADER = "ScaleProtocol.h"


# ── Build helpers ─────────────────────────────────────────────────────────────

def _pio_ini(board: str) -> str:
    return f"""\
[env:{board}]
platform     = {PLATFORM}
board        = {board}
framework    = arduino
lib_deps     = {HX711_LIB}
build_flags  = -std=gnu++17
"""


def build_sketch(label: str, ino_path: Path, board: str,
                 tmp_root: Path) -> int:
    """
    Create a throwaway PlatformIO project in tmp_root/<label>/,
    copy the sketch + shared header into src/, write platformio.ini,
    and run `pio run`.

    Returns the process exit code (0 = success).
    """
    proj = tmp_root / label
    src  = proj / "src"
    src.mkdir(parents=True)

    shutil.copy(ino_path,                          src / ino_path.name)
    shutil.copy(UPGRADES / PROTOCOL_HEADER,        src / PROTOCOL_HEADER)

    (proj / "platformio.ini").write_text(_pio_ini(board), encoding="utf-8")

    print(f"\n{'='*60}")
    print(f"  Building: {label}  ({ino_path.name})")
    print(f"  Board   : {board}")
    print(f"  Dir     : {proj}")
    print(f"{'='*60}")

    try:
        result = subprocess.run(
            _pio_cmd() + ["run", "--project-dir", str(proj)],
            text=True,
        )
        return result.returncode
    except FileNotFoundError:
        print(
            "\nERROR: PlatformIO not found (neither 'pio' on PATH nor the\n"
            "'platformio' Python module).\n"
            "Install PlatformIO Core:\n"
            "  pip install platformio\n"
            "  or: https://docs.platformio.org/en/latest/core/installation/\n"
        )
        return 127


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compile both v1.1 sketches via PlatformIO.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--board",
        default=DEFAULT_BOARD,
        metavar="BOARD_ID",
        help=f"PlatformIO board ID (default: {DEFAULT_BOARD}). "
             "Override when you know your exact hardware variant.",
    )
    args = parser.parse_args()

    missing = [
        name for _, name in SKETCHES
        if not (UPGRADES / name).exists()
    ]
    missing += [PROTOCOL_HEADER] if not (UPGRADES / PROTOCOL_HEADER).exists() else []
    if missing:
        print(f"ERROR: missing source files: {missing}")
        print(f"       Expected in: {UPGRADES}")
        return 1

    failed = []
    with tempfile.TemporaryDirectory(prefix="scales_pio_") as tmp:
        tmp_root = Path(tmp)
        for label, ino_name in SKETCHES:
            rc = build_sketch(label, UPGRADES / ino_name, args.board, tmp_root)
            if rc != 0:
                failed.append(f"{label} (rc={rc})")

    print()
    if failed:
        print(f"BUILD FAILED: {failed}")
        return 1

    print("All builds passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
