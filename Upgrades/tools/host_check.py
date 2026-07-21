#!/usr/bin/env python3
"""
host_check.py — Pure-host checks for ScaleProtocol layout and source invariants.

No Arduino toolchain required.  Tests:
  1. ScalePacket is exactly 12 bytes (ctypes struct mirror).
  2. scalePacketValid rejects wrong length, bad magic0, bad magic1,
     bad version, unknown/FL pad ID (0), and accepts all valid IDs.
  3. Source invariants in ScaleProtocol.h, both .ino files.

Run from any directory:
  python tools/host_check.py
  python Upgrades/tools/host_check.py
"""

import ctypes
import struct
import sys
from pathlib import Path

# Locate Upgrades/ regardless of cwd
_HERE    = Path(__file__).resolve().parent          # Upgrades/tools/
UPGRADES = _HERE.parent                             # Upgrades/

SP_MAGIC0  = 0xD1
SP_MAGIC1  = 0xCE
SP_VERSION = 0x11
SP_LEN     = 12

VALID_PAD_IDS = (1, 2, 3)   # FR=1, RL=2, RR=3


# ── 1. Packet layout ──────────────────────────────────────────────────────────

class ScalePacket(ctypes.LittleEndianStructure):
    _pack_   = 1
    _fields_ = [
        ("magic0",  ctypes.c_uint8),
        ("magic1",  ctypes.c_uint8),
        ("version", ctypes.c_uint8),
        ("padId",   ctypes.c_uint8),
        ("raw",     ctypes.c_float),
        ("battery", ctypes.c_float),
    ]


def test_packet_size():
    sz = ctypes.sizeof(ScalePacket)
    assert sz == SP_LEN, \
        f"ScalePacket is {sz} bytes, expected {SP_LEN}"


# ── 2. Validator logic (Python mirror of scalePacketValid) ───────────────────

def _valid(buf: bytes) -> bool:
    """Python mirror of scalePacketValid() in ScaleProtocol.h."""
    if len(buf) != SP_LEN:   return False
    if buf[0] != SP_MAGIC0:  return False
    if buf[1] != SP_MAGIC1:  return False
    if buf[2] != SP_VERSION: return False
    return buf[3] in VALID_PAD_IDS


def _make(pad_id=1, raw=12345.0, batt=3.8) -> bytes:
    return struct.pack("<BBBBff", SP_MAGIC0, SP_MAGIC1, SP_VERSION,
                       pad_id, raw, batt)


def test_validator():
    good = _make()
    assert _valid(good), "valid packet rejected"

    # Wrong length
    assert not _valid(good[:11]),          "short packet accepted"
    assert not _valid(good + b'\x00'),     "long packet accepted"
    assert not _valid(b''),                "empty packet accepted"

    # Bad magic0
    bad = bytearray(good); bad[0] ^= 0xFF
    assert not _valid(bytes(bad)),         "bad magic0 accepted"

    # Bad magic1
    bad = bytearray(good); bad[1] ^= 0xFF
    assert not _valid(bytes(bad)),         "bad magic1 accepted"

    # Wrong version (v1.0)
    bad = bytearray(good); bad[2] = 0x10
    assert not _valid(bytes(bad)),         "v1.0 version byte accepted"

    # padId = 0 — FL is local, must never appear on the wire
    bad = bytearray(good); bad[3] = 0
    assert not _valid(bytes(bad)),         "padId=0 (FL) accepted"

    # padId = 4 — unknown
    bad = bytearray(good); bad[3] = 4
    assert not _valid(bytes(bad)),         "padId=4 (unknown) accepted"

    # All three valid pad IDs must pass
    for pid in VALID_PAD_IDS:
        assert _valid(_make(pad_id=pid)), f"valid padId={pid} rejected"


# ── 3. Source invariants ──────────────────────────────────────────────────────

def _src(name: str) -> str:
    return (UPGRADES / name).read_text(encoding="utf-8")


def test_source_invariants():
    child  = _src("diy_race_scale_children_esp32.ino")
    parent = _src("diy_race_scale_parent_esp32.ino")
    header = _src("ScaleProtocol.h")

    # ── ScaleProtocol.h ──────────────────────────────────────────────
    assert "static_assert" in header, \
        "ScaleProtocol.h: missing static_assert"
    assert "SP_PACKET_LEN" in header, \
        "ScaleProtocol.h: missing SP_PACKET_LEN constant"
    assert "#define SP_PACKET_LEN  12u" in header, \
        "ScaleProtocol.h: SP_PACKET_LEN must be exactly 12u"
    assert "scalePacketValid" in header, \
        "ScaleProtocol.h: missing scalePacketValid function"
    assert "SP_MAGIC0" in header and "SP_MAGIC1" in header, \
        "ScaleProtocol.h: missing magic byte constants"
    assert "PadId" in header, \
        "ScaleProtocol.h: missing PadId enum"
    assert "PAD_FR" in header and "PAD_RL" in header and "PAD_RR" in header, \
        "ScaleProtocol.h: missing PadId enum values"

    # ── Child .ino ────────────────────────────────────────────────────
    assert "ScaleProtocol.h" in child, \
        "child: missing #include ScaleProtocol.h"
    assert "THIS_PAD" in child, \
        "child: missing typed THIS_PAD constant (must not use PAD_ID string)"
    assert "ScalePacket" in child, \
        "child: not using ScalePacket"
    assert "ScaleData" not in child, \
        "child: still contains legacy ScaleData"
    assert "zeroMAC" in child, \
        "child: missing zeroMAC guard variable"
    assert "FATAL" in child, \
        "child: missing FATAL startup message for all-zero master MAC"
    assert "masterAddress" in child, \
        "child: missing masterAddress declaration"

    # ── Parent .ino ───────────────────────────────────────────────────
    assert "ScaleProtocol.h" in parent, \
        "parent: missing #include ScaleProtocol.h"
    assert "xQueueCreate" in parent, \
        "parent: missing xQueueCreate"
    assert "xQueueSend" in parent, \
        "parent: missing xQueueSend"
    assert "xQueueReceive" in parent, \
        "parent: missing xQueueReceive"

    # Queue must be created BEFORE the callback is registered (file order)
    create_pos   = parent.index("xQueueCreate")
    register_pos = parent.index("esp_now_register_recv_cb")
    assert create_pos < register_pos, \
        ("parent: xQueueCreate must appear before esp_now_register_recv_cb "
         f"(found at positions {create_pos} vs {register_pos})")

    # setup must not return before the queue exists; Arduino calls loop()
    # after setup() returns, and xQueueReceive(nullptr, ...) would crash.
    init_start = parent.index("if (esp_now_init() != ESP_OK)")
    init_end = parent.index("rxQueue = xQueueCreate", init_start)
    init_failure_slice = parent[init_start:init_end]
    assert "return;" not in init_failure_slice, \
        "parent: ESP-NOW init failure must halt, not return into loop with null queue"
    assert "while (true)" in init_failure_slice, \
        "parent: ESP-NOW init failure must halt cleanly"

    # Callback body must NOT call pushSample — mutation belongs in loop()
    recv_start = parent.index("void onReceive")
    # The callback is compact; 900 chars is plenty to capture the body
    recv_slice = parent[recv_start : recv_start + 900]
    assert "pushSample" not in recv_slice, \
        "parent: onReceive callback must not call pushSample (queue discipline)"

    # trimmedRawMean must use automatic (stack) buffer, not static
    assert "static float buf" not in parent, \
        "parent: trimmedRawMean scratch must be automatic, not 'static float buf'"

    # padOnline must not keep FL online via scalePresent alone
    assert "return scalePresent" not in parent, \
        "parent: padOnline must not use 'return scalePresent' for FL online status"

    # Legacy ScaleData must be gone
    assert "ScaleData" not in parent, \
        "parent: legacy ScaleData still present"

    # ESP_ARDUINO_VERSION_MAJOR compat guard for onReceive signature
    assert "ESP_ARDUINO_VERSION_MAJOR" in parent, \
        "parent: missing ESP_ARDUINO_VERSION_MAJOR compat guard for onReceive"


# ── runner ────────────────────────────────────────────────────────────────────

TESTS = [
    ("1. ScalePacket size (12 bytes)",     test_packet_size),
    ("2. scalePacketValid logic",          test_validator),
    ("3. Source invariants",               test_source_invariants),
]


def main() -> int:
    print(f"Host checks — Upgrades dir: {UPGRADES}")
    print()
    failed = []
    for name, fn in TESTS:
        try:
            fn()
            print(f"  PASS  {name}")
        except AssertionError as exc:
            print(f"  FAIL  {name}")
            print(f"        {exc}")
            failed.append(name)
        except FileNotFoundError as exc:
            print(f"  ERROR {name}: source file not found — {exc}")
            failed.append(name)
        except Exception as exc:
            print(f"  ERROR {name}: {type(exc).__name__}: {exc}")
            failed.append(name)

    print()
    if failed:
        print(f"FAILED ({len(failed)}/{len(TESTS)}): {failed}")
        return 1
    print(f"All {len(TESTS)} checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
