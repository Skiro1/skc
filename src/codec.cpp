#include "skc/codec.hpp"
#include "skc/varint.hpp"
#include "skc/zstd_wrapper.hpp"
#include <cstring>

namespace skc {

#include <algorithm>
#include <cmath>

// ══════════════════════════════════════════════════════════════════════
// SKC Compressed Format — Core Codec
// ══════════════════════════════════════════════════════════════════════

// ─── WriteStream ──────────────────────────────────────────────────────
void WriteStream::putvar(uint64_t v) { varint_encode(buf, v); }
void WriteStream::putf32(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    buf.insert(buf.end(), (uint8_t*)&u, (uint8_t*)&u + 4);
}
void WriteStream::putf64(double d) {
    uint64_t u; memcpy(&u, &d, 8);
    buf.insert(buf.end(), (uint8_t*)&u, (uint8_t*)&u + 8);
}
void WriteStream::putu32(uint32_t u) {
    buf.insert(buf.end(), (uint8_t*)&u, (uint8_t*)&u + 4);
}

// ─── ReadStream ───────────────────────────────────────────────────────
bool ReadStream::getvar(uint64_t& v) {
    size_t consumed = varint_decode(data + pos, size - pos, v);
    if (!consumed) { pos = size; return false; }
    pos += consumed;
    return true;
}
float ReadStream::getf32() {
    if (pos + 4 > size) { pos = size; return 0; }
    float f; memcpy(&f, data + pos, 4); pos += 4; return f;
}
double ReadStream::getf64() {
    if (pos + 8 > size) { pos = size; return 0; }
    double d; memcpy(&d, data + pos, 8); pos += 8; return d;
}
uint32_t ReadStream::getu32() {
    if (pos + 4 > size) { pos = size; return 0; }
    uint32_t u; memcpy(&u, data + pos, 4); pos += 4; return u;
}

// ─── Float delta helpers (lossless: store uint32 XOR delta) ───────────
static int64_t float_delta(float cur, float prev) {
    uint32_t cu, pv;
    memcpy(&cu, &cur, 4);
    memcpy(&pv, &prev, 4);
    return (int64_t)(int32_t)(cu - pv);
}
static float float_add_delta(float base, int64_t delta) {
    uint32_t b; memcpy(&b, &base, 4);
    b = (uint32_t)((int32_t)b + (int32_t)delta);
    float f; memcpy(&f, &b, 4); return f;
}
static int64_t double_delta(double cur, double prev) {
    uint64_t cu, pv;
    memcpy(&cu, &cur, 8);
    memcpy(&pv, &prev, 8);
    return (int64_t)(cu - pv);
}
static double double_add_delta(double base, int64_t delta) {
    uint64_t b; memcpy(&b, &base, 8);
    b = (uint64_t)((int64_t)b + delta);
    double d; memcpy(&d, &b, 8); return d;
}

// ─── Static tracking: which fields are worth Dense vs Sparse ──────────
enum FieldClass { DENSE, SPARSE };

static FieldClass classifyField(const char* name) {
    struct { const char* n; FieldClass c; } table[] = {
        {"x", DENSE}, {"y", DENSE}, {"y_velocity", DENSE},
        {"rotation", DENSE}, {"fall_speed", DENSE},
        {"rotation_speed", SPARSE}, {"gravity", SPARSE},
        {"vehicle_size", SPARSE}, {"player_speed", SPARSE},
        {"platformer_x_velocity", SPARSE}, {"last_land_time", SPARSE},
        {"slope_rotation", SPARSE}, {"flags", SPARSE},
    };
    for (auto& [n, c] : table)
        if (strcmp(name, n) == 0) return c;
    return SPARSE;
}

// ══════════════════════════════════════════════════════════════════════
// COMPRESS
// ══════════════════════════════════════════════════════════════════════
SKCCompressResult skc_compress_v4(const Macro& macro) {
    SKCCompressResult result;
    size_t originalSize = macro.physics.size() * sizeof(PhysicsFrame)
                        + macro.inputs.size() * sizeof(Input);

    uint32_t chunkSize = 480;
    uint32_t chunkCount = (uint32_t)((macro.physics.size() + chunkSize - 1) / chunkSize);
    if (chunkCount == 0) chunkCount = 1;

    // ── PASS 1: Compress all chunks to temporary buffers ─────────────┐
    struct CompressedChunk {
        std::vector<uint8_t> data;
        uint32_t frameCount;
    };
    std::vector<CompressedChunk> chunks(chunkCount);

    auto compressChunk = [&](uint32_t ci, WriteStream& ws) -> uint32_t {
        size_t frameStart = (size_t)ci * chunkSize;
        uint32_t framesInChunk = (uint32_t)std::min<uint64_t>(
            chunkSize, macro.physics.size() - frameStart);

        // ── Compute field mask ──────────────────────────────────────────
        PhysicsFrame prevPf = {};
        if (ci > 0) {
            size_t ps = (size_t)(ci - 1) * chunkSize;
            uint32_t pc = (uint32_t)std::min<uint64_t>(chunkSize, macro.physics.size() - ps);
            prevPf = macro.physics[ps + pc - 1];
        }

        auto differs = [&](auto PhysicsFrame::*field) -> bool {
            for (uint32_t f = 0; f < framesInChunk; f++)
                if (!(macro.physics[frameStart + f].*field == prevPf.*field)) return true;
            return false;
        };

        uint32_t fieldMask = 0;
        // Frame: always written (dense varint deltas)
        fieldMask |= 1u << BIT_FRAME;
        if (differs(&PhysicsFrame::p1_x))                          fieldMask |= 1u << BIT_P1_X;
        if (differs(&PhysicsFrame::p1_y))                          fieldMask |= 1u << BIT_P1_Y;
        if (differs(&PhysicsFrame::p1_y_velocity))                 fieldMask |= 1u << BIT_P1_YV;
        if (differs(&PhysicsFrame::p1_rotation))                   fieldMask |= 1u << BIT_P1_ROT;
        if (differs(&PhysicsFrame::p1_fall_speed))                 fieldMask |= 1u << BIT_P1_FALL;
        if (differs(&PhysicsFrame::p1_gravity))                    fieldMask |= 1u << BIT_P1_GRAVITY;
        if (differs(&PhysicsFrame::p1_vehicle_size))               fieldMask |= 1u << BIT_P1_VSIZE;
        if (differs(&PhysicsFrame::p1_player_speed))               fieldMask |= 1u << BIT_P1_SPEED;
        if (differs(&PhysicsFrame::p1_platformer_x_velocity))      fieldMask |= 1u << BIT_P1_PLAT_XV;
        if (differs(&PhysicsFrame::p1_rotation_speed))             fieldMask |= 1u << BIT_P1_ROT_SPD;
        if (differs(&PhysicsFrame::p1_slope_rotation))             fieldMask |= 1u << BIT_P1_SLOPE;
        if (differs(&PhysicsFrame::p1_last_land_time))             fieldMask |= 1u << BIT_P1_LAND_T;
        if (differs(&PhysicsFrame::p1_flags))                      fieldMask |= 1u << BIT_P1_FLAGS;
        if (differs(&PhysicsFrame::p2_x))                          fieldMask |= 1u << BIT_P2_X;
        if (differs(&PhysicsFrame::p2_y))                          fieldMask |= 1u << BIT_P2_Y;
        if (differs(&PhysicsFrame::p2_y_velocity))                 fieldMask |= 1u << BIT_P2_YV;
        if (differs(&PhysicsFrame::p2_rotation))                   fieldMask |= 1u << BIT_P2_ROT;
        if (differs(&PhysicsFrame::p2_platformer_x_velocity))      fieldMask |= 1u << BIT_P2_PLAT_XV;
        if (differs(&PhysicsFrame::p2_rotation_speed))             fieldMask |= 1u << BIT_P2_ROT_SPD;
        if (differs(&PhysicsFrame::p2_gravity))                    fieldMask |= 1u << BIT_P2_GRAVITY;
        if (differs(&PhysicsFrame::p2_vehicle_size))               fieldMask |= 1u << BIT_P2_VSIZE;
        if (differs(&PhysicsFrame::p2_player_speed))               fieldMask |= 1u << BIT_P2_SPEED;
        if (differs(&PhysicsFrame::p2_fall_speed))                 fieldMask |= 1u << BIT_P2_FALL;
        if (differs(&PhysicsFrame::p2_last_land_time))             fieldMask |= 1u << BIT_P2_LAND_T;
        if (differs(&PhysicsFrame::p2_slope_rotation))             fieldMask |= 1u << BIT_P2_SLOPE;
        if (differs(&PhysicsFrame::p2_flags))                      fieldMask |= 1u << BIT_P2_FLAGS;
        // BIT_P2_PRESENT: set if any P2 field differs
        if (fieldMask & ((1u << BIT_P2_X) | (1u << BIT_P2_Y) | (1u << BIT_P2_YV) |
                         (1u << BIT_P2_ROT) | (1u << BIT_P2_PLAT_XV) | (1u << BIT_P2_ROT_SPD) |
                         (1u << BIT_P2_GRAVITY) | (1u << BIT_P2_VSIZE) | (1u << BIT_P2_SPEED) |
                         (1u << BIT_P2_FALL) | (1u << BIT_P2_LAND_T) | (1u << BIT_P2_SLOPE) |
                         (1u << BIT_P2_FLAGS)))
            fieldMask |= 1u << BIT_P2_PRESENT;

        ws.putu32(fieldMask);

        // ── Frame (dense uint64 varint deltas) ─────────────────────────
        ws.putvar(macro.physics[frameStart].frame);
        for (uint32_t f = 1; f < framesInChunk; f++) {
            uint64_t delta = macro.physics[frameStart + f].frame
                           - macro.physics[frameStart + f - 1].frame;
            ws.putvar(delta);
        }

        // ── Conditionally write fields ─────────────────────────────────
        // P1 Dense streams (float/double: XOR delta on raw bits)
        auto putDenseFloat = [&](float PhysicsFrame::*field, size_t start, uint32_t count) {
            float v0 = macro.physics[start].*field;
            ws.putf32(v0);
            if (count >= 2) {
                float v1 = macro.physics[start + 1].*field;
                ws.putvar(zig64(float_delta(v1, v0)));
                for (uint32_t f = 2; f < count; f++) {
                    float cur = macro.physics[start + f].*field;
                    float pred = 2.0f * v1 - v0;
                    ws.putvar(zig64(float_delta(cur, pred)));
                    v0 = v1; v1 = cur;
                }
            }
        };
        if (fieldMask & (1u << BIT_P1_X))
            putDenseFloat(&PhysicsFrame::p1_x, frameStart, framesInChunk);
        if (fieldMask & (1u << BIT_P1_Y))
            putDenseFloat(&PhysicsFrame::p1_y, frameStart, framesInChunk);
        if (fieldMask & (1u << BIT_P1_YV)) {
            double v0 = macro.physics[frameStart].p1_y_velocity;
            ws.putf64(v0);
            if (framesInChunk >= 2) {
                double v1 = macro.physics[frameStart + 1].p1_y_velocity;
                ws.putvar(zig64(double_delta(v1, v0)));
                for (uint32_t f = 2; f < framesInChunk; f++) {
                    double cur = macro.physics[frameStart + f].p1_y_velocity;
                    double pred = 2.0 * v1 - v0;
                    ws.putvar(zig64(double_delta(cur, pred)));
                    v0 = v1; v1 = cur;
                }
            }
        }
        if (fieldMask & (1u << BIT_P1_ROT))
            putDenseFloat(&PhysicsFrame::p1_rotation, frameStart, framesInChunk);
        if (fieldMask & (1u << BIT_P1_FALL)) {
            double v0 = macro.physics[frameStart].p1_fall_speed;
            ws.putf64(v0);
            if (framesInChunk >= 2) {
                double v1 = macro.physics[frameStart + 1].p1_fall_speed;
                ws.putvar(zig64(double_delta(v1, v0)));
                for (uint32_t f = 2; f < framesInChunk; f++) {
                    double cur = macro.physics[frameStart + f].p1_fall_speed;
                    double pred = 2.0 * v1 - v0;
                    ws.putvar(zig64(double_delta(cur, pred)));
                    v0 = v1; v1 = cur;
                }
            }
        }

        // P1 Sparse streams
        auto putSparseFloat = [&](float PhysicsFrame::*field, size_t start, uint32_t count) {
            uint64_t evCount = 1;
            for (uint32_t f = 1; f < count; f++) {
                if ((macro.physics[start + f].*field) != (macro.physics[start + f - 1].*field))
                    evCount++;
            }
            ws.putvar(evCount);
            ws.putf32(macro.physics[start].*field);
            uint64_t fa = 0;
            for (uint32_t f = 1; f < count; f++) {
                float cur = macro.physics[start + f].*field;
                float prv = macro.physics[start + f - 1].*field;
                int64_t d = float_delta(cur, prv);
                if (d == 0) continue;
                ws.putvar(zig64((int64_t)(f - fa)));
                ws.putvar(zig64(d));
                fa = f;
            }
        };
        auto putSparseDouble = [&](double PhysicsFrame::*field, size_t start, uint32_t count) {
            uint64_t evCount = 1;
            for (uint32_t f = 1; f < count; f++) {
                if ((macro.physics[start + f].*field) != (macro.physics[start + f - 1].*field))
                    evCount++;
            }
            ws.putvar(evCount);
            ws.putf64(macro.physics[start].*field);
            uint64_t fa = 0;
            for (uint32_t f = 1; f < count; f++) {
                double cur = macro.physics[start + f].*field;
                double prv = macro.physics[start + f - 1].*field;
                int64_t d = double_delta(cur, prv);
                if (d == 0) continue;
                ws.putvar(zig64((int64_t)(f - fa)));
                ws.putvar(zig64(d));
                fa = f;
            }
        };

        if (fieldMask & (1u << BIT_P1_GRAVITY))
            putSparseFloat(&PhysicsFrame::p1_gravity, frameStart, framesInChunk);
        if (fieldMask & (1u << BIT_P1_VSIZE))
            putSparseFloat(&PhysicsFrame::p1_vehicle_size, frameStart, framesInChunk);
        if (fieldMask & (1u << BIT_P1_SPEED))
            putSparseFloat(&PhysicsFrame::p1_player_speed, frameStart, framesInChunk);
        if (fieldMask & (1u << BIT_P1_PLAT_XV))
            putSparseDouble(&PhysicsFrame::p1_platformer_x_velocity, frameStart, framesInChunk);
        if (fieldMask & (1u << BIT_P1_ROT_SPD))
            putSparseFloat(&PhysicsFrame::p1_rotation_speed, frameStart, framesInChunk);
        if (fieldMask & (1u << BIT_P1_SLOPE))
            putSparseFloat(&PhysicsFrame::p1_slope_rotation, frameStart, framesInChunk);
        if (fieldMask & (1u << BIT_P1_LAND_T))
            putSparseDouble(&PhysicsFrame::p1_last_land_time, frameStart, framesInChunk);

        // P1 flags (uint32 sparse)
        if (fieldMask & (1u << BIT_P1_FLAGS)) {
            uint64_t evCount = 1;
            for (uint32_t f = 1; f < framesInChunk; f++)
                if (macro.physics[frameStart + f].p1_flags != macro.physics[frameStart + f - 1].p1_flags)
                    evCount++;
            ws.putvar(evCount);
            ws.putu32(macro.physics[frameStart].p1_flags);
            uint64_t fa = 0;
            for (uint32_t f = 1; f < framesInChunk; f++) {
                uint32_t cur = macro.physics[frameStart + f].p1_flags;
                uint32_t prv = macro.physics[frameStart + f - 1].p1_flags;
                if (cur == prv) continue;
                ws.putvar(zig64((int64_t)(f - fa)));
                ws.putvar(zig64((int64_t)(int32_t)(cur - prv)));
                fa = f;
            }
        }

        // ── P2 ───────────────────────────────────────────────────────────
        if (fieldMask & (1u << BIT_P2_PRESENT)) {
            if (fieldMask & (1u << BIT_P2_X))
                putDenseFloat(&PhysicsFrame::p2_x, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_Y))
                putDenseFloat(&PhysicsFrame::p2_y, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_YV)) {
                double v0 = macro.physics[frameStart].p2_y_velocity;
                ws.putf64(v0);
                if (framesInChunk >= 2) {
                    double v1 = macro.physics[frameStart + 1].p2_y_velocity;
                    ws.putvar(zig64(double_delta(v1, v0)));
                    for (uint32_t f = 2; f < framesInChunk; f++) {
                        double cur = macro.physics[frameStart + f].p2_y_velocity;
                        double pred = 2.0 * v1 - v0;
                        ws.putvar(zig64(double_delta(cur, pred)));
                        v0 = v1; v1 = cur;
                    }
                }
            }
            if (fieldMask & (1u << BIT_P2_ROT))
                putDenseFloat(&PhysicsFrame::p2_rotation, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_PLAT_XV))
                putSparseDouble(&PhysicsFrame::p2_platformer_x_velocity, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_ROT_SPD))
                putSparseFloat(&PhysicsFrame::p2_rotation_speed, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_GRAVITY))
                putSparseFloat(&PhysicsFrame::p2_gravity, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_VSIZE))
                putSparseFloat(&PhysicsFrame::p2_vehicle_size, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_SPEED))
                putSparseFloat(&PhysicsFrame::p2_player_speed, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_FALL))
                putSparseDouble(&PhysicsFrame::p2_fall_speed, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_LAND_T))
                putSparseDouble(&PhysicsFrame::p2_last_land_time, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_SLOPE))
                putSparseFloat(&PhysicsFrame::p2_slope_rotation, frameStart, framesInChunk);
            if (fieldMask & (1u << BIT_P2_FLAGS)) {
                uint64_t evCount = 1;
                for (uint32_t f = 1; f < framesInChunk; f++)
                    if (macro.physics[frameStart + f].p2_flags != macro.physics[frameStart + f - 1].p2_flags)
                        evCount++;
                ws.putvar(evCount);
                ws.putu32(macro.physics[frameStart].p2_flags);
                uint64_t fa = 0;
                for (uint32_t f = 1; f < framesInChunk; f++) {
                    uint32_t cur = macro.physics[frameStart + f].p2_flags;
                    uint32_t prv = macro.physics[frameStart + f - 1].p2_flags;
                    if (cur == prv) continue;
                    ws.putvar(zig64((int64_t)(f - fa)));
                    ws.putvar(zig64((int64_t)(int32_t)(cur - prv)));
                    fa = f;
                }
            }
        }

        return framesInChunk;
    };

    // Compress each chunk to temporary buffer
    for (uint32_t ci = 0; ci < chunkCount; ci++) {
        WriteStream cws;
        chunks[ci].frameCount = compressChunk(ci, cws);
        chunks[ci].data = std::move(cws.buf);
    }

    // ── PASS 2: Serialize body (metadata + chunk table + inputs + chunks) ──
    WriteStream body;

    auto writeStr = [&](WriteStream& s, const std::string& str) {
        s.putvar(str.size());
        s.buf.insert(s.buf.end(), str.begin(), str.end());
    };
    writeStr(body, macro.author);
    writeStr(body, macro.description);
    writeStr(body, macro.level_name);

    // Inputs
    body.putvar(macro.inputs.size());
    {
        uint64_t pf = 0;
        for (auto& inp : macro.inputs) {
            body.putvar(inp.frame - pf);
            uint8_t packed = (uint8_t)inp.button | (inp.player2 ? 0x08 : 0) | (inp.down ? 0x10 : 0);
            body.buf.push_back(packed);
            pf = inp.frame;
        }
    }

    // Chunks (sequentially after inputs, no table)
    for (auto& chunk : chunks)
        body.buf.insert(body.buf.end(), chunk.data.begin(), chunk.data.end());

    // Visual anchor blobs (v5): sparse full-state snapshots for playback
    // visual/animation fidelity. Appended at the end so v4 files (no anchors)
    // simply have no trailing bytes.
    body.putvar(macro.visualAnchors.size());
    for (auto& blob : macro.visualAnchors) {
        body.putvar((uint64_t)blob.size());
        body.buf.insert(body.buf.end(), blob.begin(), blob.end());
    }

    // Gameplay loops (trailing, optional; readers unaware of loops simply
    // ignore the extra bytes). Each loop references its pattern by index into
    // `inputs`, which for a stored macro is the flat expanded base; the bot's
    // expandLoopsInto handles both flat and deduplicated input arrays.
    body.putvar(macro.loops.size());
    for (auto& loop : macro.loops) {
        body.putvar(loop.startFrame);
        body.putvar(loop.patternLen);
        body.putvar(loop.repeatCount);
        body.putvar(loop.delayBetweenRepeats);
        body.putvar(loop.inputStart);
        body.putvar(loop.inputEnd);
    }

    // ── PASS 3: Compress body with Zstd ────────────────────────────────
    std::vector<uint8_t> compressedBody = zstd_compress(body.buf, 19);

    // ── PASS 4: Write final file ────────────────────────────────────────
    WriteStream ws;
    const char magic[] = "SKC3";
    ws.buf.insert(ws.buf.end(), magic, magic + 4);
    ws.putu32(5); // v5: Zstd-compressed + visual anchor section
    ws.putu32(SKC_FLAG_ZSTD); // flags: Zstd, no XOR
    ws.putvar((uint64_t)macro.level_id);
    ws.putvar(macro.seed);
    ws.putvar(macro.physics.size());
    ws.putvar(macro.physics.empty() ? 0 : macro.physics[0].frame);
    ws.putu32(chunkSize);
    ws.putu32(chunkCount);
    ws.putvar((uint64_t)compressedBody.size());
    ws.buf.insert(ws.buf.end(), compressedBody.begin(), compressedBody.end());

    result.data = std::move(ws.buf);
    result.compression_ratio = originalSize > 0
        ? (double)result.data.size() / originalSize
        : 1.0;

    return result;
}



// ══════════════════════════════════════════════════════════════════════
// DECOMPRESS (v4 + v5 dispatcher)
// ══════════════════════════════════════════════════════════════════════
bool skc_decompress(const std::vector<uint8_t>& data, Macro& macro) {
    if (data.size() < 8) return false;

    char magic[4];
    memcpy(magic, data.data(), 4);

    if (memcmp(magic, "SKC3", 4) != 0) return false;

    // ── v4 decompress ─────────────────────────────────────────────────────
    ReadStream rs;
    rs.data = data.data();
    rs.size = data.size();
    if (rs.size < 28) return false;
    rs.pos += 4; // skip magic

    uint32_t version = rs.getu32();
    if (version != 5) return false; // only v5 (Zstd chunked + visual anchors) is supported
    uint32_t flags = rs.getu32();

    // Decompress
    std::vector<uint8_t> decompressed;
    uint64_t totalFrames = 0;
    uint32_t chunkSize = 0;
    uint32_t chunkCount = 0;

    if (!(flags & SKC_FLAG_ZSTD)) return false;

    uint64_t lid; if (!rs.getvar(lid)) return false; macro.level_id = (int)lid;
    if (!rs.getvar(macro.seed)) return false;
    if (!rs.getvar(totalFrames)) return false;
    uint64_t baseFrame = 0;
    if (!rs.getvar(baseFrame)) return false;
    chunkSize = rs.getu32();
    chunkCount = rs.getu32();
    uint64_t bodySize; if (!rs.getvar(bodySize)) return false;

    if (rs.pos + bodySize > rs.size) return false;
    std::vector<uint8_t> bodyData(rs.data + rs.pos, rs.data + rs.pos + (size_t)bodySize);
    rs.pos += (size_t)bodySize;

    decompressed = zstd_decompress(bodyData);
    if (decompressed.empty()) return false;

        // Replace rs with decompressed body
        rs.data = decompressed.data();
        rs.size = decompressed.size();
        rs.pos = 0;
        macro.tps = 240.0f;

        // Parse body: strings
        auto readStr = [&](std::string& s) {
            uint64_t len;
            if (!rs.getvar(len)) return;
            s.resize((size_t)len);
            if (rs.pos + len <= rs.size) {
                memcpy(&s[0], rs.data + rs.pos, (size_t)len);
                rs.pos += (size_t)len;
            }
        };
        readStr(macro.author);
        readStr(macro.description);
        readStr(macro.level_name);

        // Inputs
        uint64_t inputCount;
        if (!rs.getvar(inputCount)) return false;
        macro.inputs.resize((size_t)inputCount);
        uint64_t prevFrame = 0;
        for (size_t i = 0; i < (size_t)inputCount; i++) {
            uint64_t frameDelta;
            if (!rs.getvar(frameDelta)) return false;
            macro.inputs[i].frame = prevFrame + frameDelta;
            if (rs.pos >= rs.size) return false;
            uint8_t packed = rs.data[rs.pos++];
            macro.inputs[i].button = (Button)(packed & 0x07);
            macro.inputs[i].player2 = (packed & 0x08) != 0;
            macro.inputs[i].down = (packed & 0x10) != 0;
            prevFrame = macro.inputs[i].frame;
        }

        // Decompress chunks
        macro.physics.reserve((size_t)totalFrames);
        PhysicsFrame prevPf = {};

        for (uint32_t ci = 0; ci < chunkCount; ci++) {
            uint64_t framesInChunk = std::min<uint64_t>(chunkSize, totalFrames - (uint64_t)ci * chunkSize);
            if (framesInChunk == 0) continue;

            std::vector<PhysicsFrame> chunkFrames((size_t)framesInChunk);
            // Set default from prev chunk's last frame
            if (ci > 0) {
            for (auto& pf : chunkFrames) pf = prevPf;
            }

            // ── Read field mask ──────────────────────────────────────────────
            uint32_t fieldMask = rs.getu32();

            // ── Frame (dense uint64 varint deltas) ───────────────────────────
            if (fieldMask & (1u << BIT_FRAME)) {
            uint64_t fv; if (!rs.getvar(fv)) return false;
            chunkFrames[0].frame = fv;
            for (uint32_t f = 1; f < framesInChunk; f++) {
                uint64_t delta; if (!rs.getvar(delta)) return false;
                chunkFrames[f].frame = chunkFrames[f-1].frame + delta;
            }
            } else {
            for (uint32_t f = 0; f < framesInChunk; f++)
                chunkFrames[f].frame = baseFrame + (uint64_t)ci * chunkSize + f;
            }

            // ── P1 Dense: x (predictive XOR delta) ─────────────────────────
            if (fieldMask & (1u << BIT_P1_X)) {
            float v0 = rs.getf32();
            chunkFrames[0].p1_x = v0;
            if (framesInChunk >= 2) {
                uint64_t zd; if (!rs.getvar(zd)) return false;
                float v1 = float_add_delta(v0, unzig64(zd));
                chunkFrames[1].p1_x = v1;
                for (uint32_t f = 2; f < framesInChunk && rs.ok(); f++) {
                    if (!rs.getvar(zd)) return false;
                    int64_t d = unzig64(zd);
                    float pred = 2.0f * v1 - v0;
                    float val = float_add_delta(pred, d);
                    chunkFrames[f].p1_x = val;
                    v0 = v1; v1 = val;
                }
            }
            }

            // ── P1 Dense: y (predictive XOR delta) ─────────────────────────
            if (fieldMask & (1u << BIT_P1_Y)) {
            float v0 = rs.getf32();
            chunkFrames[0].p1_y = v0;
            if (framesInChunk >= 2) {
                uint64_t zd; if (!rs.getvar(zd)) return false;
                float v1 = float_add_delta(v0, unzig64(zd));
                chunkFrames[1].p1_y = v1;
                for (uint32_t f = 2; f < framesInChunk && rs.ok(); f++) {
                    if (!rs.getvar(zd)) return false;
                    int64_t d = unzig64(zd);
                    float pred = 2.0f * v1 - v0;
                    float val = float_add_delta(pred, d);
                    chunkFrames[f].p1_y = val;
                    v0 = v1; v1 = val;
                }
            }
            }

            // ── P1 Dense: y_velocity (predictive, stored as double) ──────────
            if (fieldMask & (1u << BIT_P1_YV)) {
            double v0 = rs.getf64();
            chunkFrames[0].p1_y_velocity = v0;
            if (framesInChunk >= 2) {
                uint64_t zd; if (!rs.getvar(zd)) return false;
                double v1 = double_add_delta(v0, unzig64(zd));
                chunkFrames[1].p1_y_velocity = v1;
                for (uint32_t f = 2; f < framesInChunk && rs.ok(); f++) {
                    if (!rs.getvar(zd)) return false;
                    int64_t d = unzig64(zd);
                    double pred = 2.0 * v1 - v0;
                    double val = double_add_delta(pred, d);
                    chunkFrames[f].p1_y_velocity = val;
                    v0 = v1; v1 = val;
                }
            }
            }

        // ── P1 Dense: rotation (predictive XOR delta) ────────────────────
        if (fieldMask & (1u << BIT_P1_ROT)) {
            float v0 = rs.getf32();
            chunkFrames[0].p1_rotation = v0;
            if (framesInChunk >= 2) {
                uint64_t zd; if (!rs.getvar(zd)) return false;
                float v1 = float_add_delta(v0, unzig64(zd));
                chunkFrames[1].p1_rotation = v1;
                for (uint32_t f = 2; f < framesInChunk && rs.ok(); f++) {
                    if (!rs.getvar(zd)) return false;
                    int64_t d = unzig64(zd);
                    float pred = 2.0f * v1 - v0;
                    float val = float_add_delta(pred, d);
                    chunkFrames[f].p1_rotation = val;
                    v0 = v1; v1 = val;
                }
            }
        }

            // ── P1 Dense: fall_speed (predictive, stored as double) ──────────
            if (fieldMask & (1u << BIT_P1_FALL)) {
            double v0 = rs.getf64();
            chunkFrames[0].p1_fall_speed = v0;
            if (framesInChunk >= 2) {
                uint64_t zd; if (!rs.getvar(zd)) return false;
                double v1 = double_add_delta(v0, unzig64(zd));
                chunkFrames[1].p1_fall_speed = v1;
                for (uint32_t f = 2; f < framesInChunk && rs.ok(); f++) {
                    if (!rs.getvar(zd)) return false;
                    int64_t d = unzig64(zd);
                    double pred = 2.0 * v1 - v0;
                    double val = double_add_delta(pred, d);
                    chunkFrames[f].p1_fall_speed = val;
                    v0 = v1; v1 = val;
                }
            }
            }

            // ── P1 Sparse fields ─────────────────────────────────────────────
            auto readSparseF32 = [&](float PhysicsFrame::*field) {
            uint64_t evCount; if (!rs.getvar(evCount)) return;
            std::vector<bool> _set((size_t)framesInChunk, false);
            uint64_t frameAccum = 0;
            float lastVal = 0;
            for (uint64_t ei = 0; ei < evCount && rs.ok(); ei++) {
                if (ei == 0) {
                    chunkFrames[0].*field = rs.getf32();
                    lastVal = chunkFrames[0].*field;
                    _set[0] = true;
                    continue;
                }
                uint64_t fo; if (!rs.getvar(fo)) return;
                uint64_t zd; if (!rs.getvar(zd)) return;
                int64_t d = unzig64(zd);
                frameAccum += unzig64(fo);
                if (frameAccum < framesInChunk) {
                    chunkFrames[(size_t)frameAccum].*field = float_add_delta(lastVal, d);
                    lastVal = chunkFrames[(size_t)frameAccum].*field;
                    _set[(size_t)frameAccum] = true;
                }
            }
            lastVal = chunkFrames[0].*field;
            for (uint32_t f = 1; f < framesInChunk; f++) {
                if (_set[f])
                    lastVal = chunkFrames[f].*field;
                else
                    chunkFrames[f].*field = lastVal;
            }
            };
            auto readSparseF64 = [&](double PhysicsFrame::*field) {
            uint64_t evCount; if (!rs.getvar(evCount)) return;
            std::vector<bool> _set((size_t)framesInChunk, false);
            uint64_t frameAccum = 0;
            double lastVal = 0;
            for (uint64_t ei = 0; ei < evCount && rs.ok(); ei++) {
                if (ei == 0) {
                    chunkFrames[0].*field = rs.getf64();
                    lastVal = chunkFrames[0].*field;
                    _set[0] = true;
                    continue;
                }
                uint64_t fo; if (!rs.getvar(fo)) return;
                uint64_t zd; if (!rs.getvar(zd)) return;
                int64_t d = unzig64(zd);
                frameAccum += unzig64(fo);
                if (frameAccum < framesInChunk) {
                    chunkFrames[(size_t)frameAccum].*field = double_add_delta(lastVal, d);
                    lastVal = chunkFrames[(size_t)frameAccum].*field;
                    _set[(size_t)frameAccum] = true;
                }
            }
            lastVal = chunkFrames[0].*field;
            for (uint32_t f = 1; f < framesInChunk; f++) {
                if (_set[f])
                    lastVal = chunkFrames[f].*field;
                else
                    chunkFrames[f].*field = lastVal;
            }
            };

            if (fieldMask & (1u << BIT_P1_GRAVITY))
            readSparseF32(&PhysicsFrame::p1_gravity);
            if (fieldMask & (1u << BIT_P1_VSIZE))
            readSparseF32(&PhysicsFrame::p1_vehicle_size);
            if (fieldMask & (1u << BIT_P1_SPEED))
            readSparseF32(&PhysicsFrame::p1_player_speed);
            if (fieldMask & (1u << BIT_P1_PLAT_XV))
            readSparseF64(&PhysicsFrame::p1_platformer_x_velocity);
            if (fieldMask & (1u << BIT_P1_ROT_SPD))
            readSparseF32(&PhysicsFrame::p1_rotation_speed);
            if (fieldMask & (1u << BIT_P1_SLOPE))
            readSparseF32(&PhysicsFrame::p1_slope_rotation);
            if (fieldMask & (1u << BIT_P1_LAND_T))
            readSparseF64(&PhysicsFrame::p1_last_land_time);

            // P1 flags (uint32 sparse)
            if (fieldMask & (1u << BIT_P1_FLAGS)) {
            std::vector<bool> _sf((size_t)framesInChunk, false);
            uint64_t evCount; if (!rs.getvar(evCount)) return false;
            uint64_t frameAccum = 0;
            uint32_t lastVal = 0;
            for (uint64_t ei = 0; ei < evCount && rs.ok(); ei++) {
                if (ei == 0) {
                    chunkFrames[0].p1_flags = rs.getu32();
                    lastVal = chunkFrames[0].p1_flags;
                    _sf[0] = true;
                    continue;
                }
                uint64_t fo; if (!rs.getvar(fo)) return false;
                uint64_t zd; if (!rs.getvar(zd)) return false;
                int64_t d = unzig64(zd);
                frameAccum += unzig64(fo);
                if (frameAccum < framesInChunk) {
                    lastVal = (uint32_t)((int32_t)lastVal + (int32_t)d);
                    chunkFrames[(size_t)frameAccum].p1_flags = lastVal;
                    _sf[(size_t)frameAccum] = true;
                }
            }
            lastVal = chunkFrames[0].p1_flags;
            for (uint32_t f = 1; f < framesInChunk; f++) {
                if (_sf[f])
                    lastVal = chunkFrames[f].p1_flags;
                else
                    chunkFrames[f].p1_flags = lastVal;
            }
            }

            // ── P2 (predictive XOR delta for dense fields) ────────────────
            if (fieldMask & (1u << BIT_P2_PRESENT)) {
            if (fieldMask & (1u << BIT_P2_X)) {
                float v0 = rs.getf32();
                chunkFrames[0].p2_x = v0;
                if (framesInChunk >= 2) {
                    uint64_t zd; if (!rs.getvar(zd)) return false;
                    float v1 = float_add_delta(v0, unzig64(zd));
                    chunkFrames[1].p2_x = v1;
                    for (uint32_t f = 2; f < framesInChunk && rs.ok(); f++) {
                        if (!rs.getvar(zd)) return false;
                        int64_t d = unzig64(zd);
                        float pred = 2.0f * v1 - v0;
                        chunkFrames[f].p2_x = float_add_delta(pred, d);
                        v0 = v1; v1 = chunkFrames[f].p2_x;
                    }
                }
            }
            if (fieldMask & (1u << BIT_P2_Y)) {
                float v0 = rs.getf32();
                chunkFrames[0].p2_y = v0;
                if (framesInChunk >= 2) {
                    uint64_t zd; if (!rs.getvar(zd)) return false;
                    float v1 = float_add_delta(v0, unzig64(zd));
                    chunkFrames[1].p2_y = v1;
                    for (uint32_t f = 2; f < framesInChunk && rs.ok(); f++) {
                        if (!rs.getvar(zd)) return false;
                        int64_t d = unzig64(zd);
                        float pred = 2.0f * v1 - v0;
                        chunkFrames[f].p2_y = float_add_delta(pred, d);
                        v0 = v1; v1 = chunkFrames[f].p2_y;
                    }
                }
            }
            if (fieldMask & (1u << BIT_P2_YV)) {
                double v0 = rs.getf64();
                chunkFrames[0].p2_y_velocity = v0;
                if (framesInChunk >= 2) {
                    uint64_t zd; if (!rs.getvar(zd)) return false;
                    double v1 = double_add_delta(v0, unzig64(zd));
                    chunkFrames[1].p2_y_velocity = v1;
                    for (uint32_t f = 2; f < framesInChunk && rs.ok(); f++) {
                        if (!rs.getvar(zd)) return false;
                        int64_t d = unzig64(zd);
                        double pred = 2.0 * v1 - v0;
                        double val = double_add_delta(pred, d);
                        chunkFrames[f].p2_y_velocity = val;
                        v0 = v1; v1 = val;
                    }
                }
            }
            if (fieldMask & (1u << BIT_P2_ROT)) {
                float v0 = rs.getf32();
                chunkFrames[0].p2_rotation = v0;
                if (framesInChunk >= 2) {
                    uint64_t zd; if (!rs.getvar(zd)) return false;
                    float v1 = float_add_delta(v0, unzig64(zd));
                    chunkFrames[1].p2_rotation = v1;
                    for (uint32_t f = 2; f < framesInChunk && rs.ok(); f++) {
                        if (!rs.getvar(zd)) return false;
                        int64_t d = unzig64(zd);
                        float pred = 2.0f * v1 - v0;
                        chunkFrames[f].p2_rotation = float_add_delta(pred, d);
                        v0 = v1; v1 = chunkFrames[f].p2_rotation;
                    }
                }
            }
            if (fieldMask & (1u << BIT_P2_PLAT_XV))
                readSparseF64(&PhysicsFrame::p2_platformer_x_velocity);
            if (fieldMask & (1u << BIT_P2_ROT_SPD))
                readSparseF32(&PhysicsFrame::p2_rotation_speed);
            if (fieldMask & (1u << BIT_P2_GRAVITY))
                readSparseF32(&PhysicsFrame::p2_gravity);
            if (fieldMask & (1u << BIT_P2_VSIZE))
                readSparseF32(&PhysicsFrame::p2_vehicle_size);
            if (fieldMask & (1u << BIT_P2_SPEED))
                readSparseF32(&PhysicsFrame::p2_player_speed);
            if (fieldMask & (1u << BIT_P2_FALL))
                readSparseF64(&PhysicsFrame::p2_fall_speed);
            if (fieldMask & (1u << BIT_P2_LAND_T))
                readSparseF64(&PhysicsFrame::p2_last_land_time);
            if (fieldMask & (1u << BIT_P2_SLOPE))
                readSparseF32(&PhysicsFrame::p2_slope_rotation);
            // P2 flags
            if (fieldMask & (1u << BIT_P2_FLAGS)) {
                std::vector<bool> _sf((size_t)framesInChunk, false);
                uint64_t evCount; if (!rs.getvar(evCount)) return false;
                uint64_t frameAccum = 0;
                uint32_t lastVal = 0;
                for (uint64_t ei = 0; ei < evCount && rs.ok(); ei++) {
                    if (ei == 0) {
                        chunkFrames[0].p2_flags = rs.getu32();
                        lastVal = chunkFrames[0].p2_flags;
                        _sf[0] = true;
                        continue;
                    }
                    uint64_t fo; if (!rs.getvar(fo)) return false;
                    uint64_t zd; if (!rs.getvar(zd)) return false;
                    int64_t d = unzig64(zd);
                    frameAccum += unzig64(fo);
                    if (frameAccum < framesInChunk) {
                        lastVal = (uint32_t)((int32_t)lastVal + (int32_t)d);
                        chunkFrames[(size_t)frameAccum].p2_flags = lastVal;
                        _sf[(size_t)frameAccum] = true;
                    }
                }
                lastVal = chunkFrames[0].p2_flags;
                for (uint32_t f = 1; f < framesInChunk; f++) {
                    if (_sf[f])
                        lastVal = chunkFrames[f].p2_flags;
                    else
                        chunkFrames[f].p2_flags = lastVal;
                }
            }
            }

            // ── Save last frame as default for next chunk ──────────────────
            prevPf = chunkFrames.back();

            // ── Append chunk ─────────────────────────────────────────────────
        macro.physics.insert(macro.physics.end(), chunkFrames.begin(), chunkFrames.end());
    }

    // ── Visual anchor blobs (v5+) ──────────────────────────────────────────
    // Only present when trailing bytes remain (v4 files have none). Each anchor
    // is an opaque serialized blob (frame + p2_exists + p1 bytes + p2 bytes).
    macro.visualAnchors.clear();
    if (rs.pos < rs.size) {
        uint64_t vaCount;
        if (rs.getvar(vaCount)) {
            for (uint64_t i = 0; i < vaCount && rs.pos < rs.size; i++) {
                uint64_t blen;
                if (!rs.getvar(blen)) break;
                if (blen > rs.size - rs.pos) break;
                macro.visualAnchors.push_back(
                    std::vector<uint8_t>(rs.data + rs.pos, rs.data + rs.pos + (size_t)blen));
                rs.pos += (size_t)blen;
            }
        }
    }

    // ── Gameplay loops (trailing; absent in older files) ───────────────────
    macro.loops.clear();
    if (rs.pos < rs.size) {
        uint64_t loopCount;
        if (rs.getvar(loopCount)) {
            for (uint64_t i = 0; i < loopCount && rs.pos < rs.size; i++) {
                uint64_t sf, pl, rc, dl, is, ie;
                if (!rs.getvar(sf) || !rs.getvar(pl) || !rs.getvar(rc) ||
                    !rs.getvar(dl) || !rs.getvar(is) || !rs.getvar(ie)) break;
                CompressedLoop loop;
                loop.startFrame = sf;
                loop.patternLen = (uint32_t)pl;
                loop.repeatCount = (uint32_t)rc;
                loop.delayBetweenRepeats = (uint32_t)dl;
                loop.inputStart = is;
                loop.inputEnd = ie;
                macro.loops.push_back(loop);
            }
        }
    }

    return true;
}

} // namespace skc
