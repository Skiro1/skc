#pragma once
#include <cstdint>
#include <vector>
#include <cassert>

// ── Zigzag: signed ↔ unsigned ──────────────────────────────────────────
inline uint32_t zig32(int32_t x)  { return (uint32_t)(x << 1) ^ (uint32_t)(x >> 31); }
inline int32_t  unzig32(uint32_t z){ return (int32_t)(z >> 1) ^ (-(int32_t)(z & 1)); }
inline uint64_t zig64(int64_t x)  { return (uint64_t)(x << 1) ^ (uint64_t)(x >> 63); }
inline int64_t  unzig64(uint64_t z){ return (int64_t)(z >> 1) ^ (-(int64_t)(z & 1)); }

// ── Varint encode ───────────────────────────────────────────────────────
inline void varint_encode(std::vector<uint8_t>& out, uint64_t v) {
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v) byte |= 0x80;
        out.push_back(byte);
    } while (v);
}

// ── Varint decode (returns bytes consumed) ──────────────────────────────
inline size_t varint_decode(const uint8_t* data, size_t size, uint64_t& out) {
    out = 0;
    size_t shift = 0;
    size_t consumed = 0;
    while (consumed < size) {
        uint8_t byte = data[consumed++];
        out |= (uint64_t)(byte & 0x7F) << shift;
        shift += 7;
        if (!(byte & 0x80)) return consumed;
    }
    // malformed
    out = 0;
    return 0;
}

// ── Float-as-uint32 helpers (lossless) ──────────────────────────────────
inline uint32_t f32_u32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline float    u32_f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint64_t f64_u64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
inline double   u64_f64(uint64_t u) { double d; memcpy(&u, &d, 8); return d; }
