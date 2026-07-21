/*
 * ScaleProtocol.h — DIY Race Scales v1.1 shared wire format
 *
 * Packet layout (13 bytes, little-endian IEEE-754 floats):
 *   [0]    magic0   = SP_MAGIC0  (0xD1)
 *   [1]    magic1   = SP_MAGIC1  (0xCE)
 *   [2]    version  = SP_VERSION (0x12)
 *   [3]    padId    = PadId enum  (FR=1, RL=2, RR=3)
 *   [4]    seq      = per-pad send counter, wraps at 255 — lets the
 *                     master measure packet loss per pad
 *   [5-8]  raw      = HX711 raw counts (float)
 *   [9-12] battery  = volts       (float)
 *
 * FL is wired directly to the master; it never appears on the wire.
 * Bumping SP_VERSION makes old firmwares hard-fail rather than
 * silently corrupt — flash all nodes together when changing this.
 *
 * Include this header in both the child and parent sketches.
 * Do NOT create a main.cpp; the .ino files are authoritative.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Constants ─────────────────────────────────────────────────────────── */

#define SP_MAGIC0      0xD1u
#define SP_MAGIC1      0xCEu
#define SP_VERSION     0x12u    /* v1.1p2 — seq field added; incompatible with 0x10/0x11 */
#define SP_PACKET_LEN  13u

/* ── Numeric pad identifiers ────────────────────────────────────────────── */
/*
 * Typed enum with fixed underlying type keeps padId unambiguous across
 * compilers and avoids accidental integer promotion. FL (value 0) is local
 * to the master and intentionally absent so the validator rejects it.
 */
enum PadId : uint8_t {
    PAD_FR = 1u,
    PAD_RL = 2u,
    PAD_RR = 3u,
};

/* ── Wire packet ────────────────────────────────────────────────────────── */

#pragma pack(push, 1)
typedef struct {
    uint8_t magic0;    /* SP_MAGIC0                         */
    uint8_t magic1;    /* SP_MAGIC1                         */
    uint8_t version;   /* SP_VERSION                        */
    uint8_t padId;     /* PadId enum value (1-3)            */
    uint8_t seq;       /* per-pad send counter (wraps)      */
    float   raw;       /* RAW HX711 counts                  */
    float   battery;   /* volts                             */
} ScalePacket;
#pragma pack(pop)

static_assert(sizeof(ScalePacket) == SP_PACKET_LEN,
              "ScalePacket size mismatch — check compiler padding");

/* ── Pure validator ─────────────────────────────────────────────────────── */
/*
 * Returns true iff buf[0..len-1] is a well-formed v1.1 packet.
 * Rejects: wrong length, bad magic bytes, wrong version, unknown/FL pad ID.
 * Does NOT interpret the float fields (NaN/Inf handled by callers).
 * Safe to call from any context including the ESP-NOW receive callback.
 */
static inline bool scalePacketValid(const uint8_t *buf, int len) {
    if ((unsigned)len != SP_PACKET_LEN) return false;
    if (buf[0] != SP_MAGIC0)            return false;
    if (buf[1] != SP_MAGIC1)            return false;
    if (buf[2] != SP_VERSION)           return false;
    uint8_t id = buf[3];
    return (id == (uint8_t)PAD_FR ||
            id == (uint8_t)PAD_RL ||
            id == (uint8_t)PAD_RR);
}
