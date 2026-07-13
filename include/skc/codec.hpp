#pragma once
#include "skc/format.hpp"
#include <vector>
#include <cstdint>
#include <string>

namespace skc {

enum SKCFlags : uint32_t {
    SKC_FLAG_ZSTD = 1u << 0,   // body is zstd-compressed
    SKC_FLAG_XOR  = 1u << 1,   // body is XOR-scrambled
};

// ─── Codec API ─────────────────────────────────────────────────────────
struct SKCCompressResult {
    std::vector<uint8_t> data;
    double compression_ratio = 0.0;  // compressed / original
};

// v5 lossless (Zstd chunked + visual anchor section)
SKCCompressResult skc_compress_v4(const Macro& macro);

// Auto-detect format by magic bytes
bool skc_decompress(const std::vector<uint8_t>& data, Macro& macro);

// ─── Stream helpers (internal, exposed for testing) ────────────────────
struct WriteStream {
    std::vector<uint8_t> buf;
    void putvar(uint64_t v);
    void putf32(float f);
    void putf64(double d);
    void putu32(uint32_t u);
};

struct ReadStream {
    const uint8_t* data = nullptr;
    size_t         size = 0;
    size_t         pos  = 0;

    bool   getvar(uint64_t& v);
    float  getf32();
    double getf64();
    uint32_t getu32();
    bool   ok() const { return pos <= size; }
};

// Field mask bits (SKC2) — matches serialization order
enum FieldBits : uint32_t {
    BIT_P1_X       = 0,  // dense f32
    BIT_P1_Y       = 1,  // dense f32
    BIT_P1_YV      = 2,  // dense f64
    BIT_P1_ROT     = 3,  // dense f32
    BIT_P1_FALL    = 4,  // dense f64
    BIT_P1_GRAVITY = 5,  // sparse f32
    BIT_P1_VSIZE   = 6,  // sparse f32
    BIT_P1_SPEED   = 7,  // sparse f32
    BIT_P1_PLAT_XV = 8,  // sparse f64
    BIT_P1_ROT_SPD = 9,  // sparse f32
    BIT_P1_SLOPE   = 10, // sparse f32
    BIT_P1_LAND_T  = 11, // sparse f64
    BIT_P1_FLAGS   = 12, // sparse u32
    BIT_P2_PRESENT = 13, // bool (redundant if any P2 bit set)
    BIT_P2_X       = 14, // dense f32
    BIT_P2_Y       = 15, // dense f32
    BIT_P2_YV      = 16, // dense f64
    BIT_P2_ROT     = 17, // dense f32
    BIT_P2_PLAT_XV = 18, // sparse f64
    BIT_P2_ROT_SPD = 19, // sparse f32
    BIT_P2_GRAVITY = 20, // sparse f32
    BIT_P2_VSIZE   = 21, // sparse f32
    BIT_P2_SPEED   = 22, // sparse f32
    BIT_P2_FALL    = 23, // sparse f64
    BIT_P2_LAND_T  = 24, // sparse f64
    BIT_P2_SLOPE   = 25, // sparse f32
    BIT_P2_FLAGS   = 26, // sparse u32
    BIT_FRAME      = 27, // dense u64 (varint deltas, replaces baseFrame+i)
};

} // namespace skc
