#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <algorithm>

// ── Mirrors the .skk format from the bot (for converter I/O) ──────────
namespace skc {

constexpr uint32_t SKK_MAGIC   = 0x424B4B53;
constexpr uint32_t SKK_VERSION = 5; // v5: + visual/animation state anchors (opaque blobs)

enum class Button : uint8_t { Jump = 1, Left = 2, Right = 3 };

struct Input {
    uint64_t frame;
    Button   button;
    bool     player2;
    bool     down;

    bool operator==(const Input& o) const {
        return frame == o.frame && button == o.button &&
               player2 == o.player2 && down == o.down;
    }
};

enum PhysicsFlags : uint32_t {
    PF_ON_GROUND    = 1u << 0,
    PF_ON_SLOPE     = 1u << 1,
    PF_DASHING      = 1u << 2,
    PF_GOING_LEFT   = 1u << 3,
    PF_UPSIDE_DOWN  = 1u << 4,
    PF_SIDEWAYS     = 1u << 5,
    PF_SHIP         = 1u << 6,
    PF_BALL         = 1u << 7,
    PF_BIRD         = 1u << 8,
    PF_DART         = 1u << 9,
    PF_ROBOT        = 1u << 10,
    PF_SPIDER       = 1u << 11,
    PF_SWING        = 1u << 12,
    PF_ACCELERATING = 1u << 13,
    PF_SLIDING      = 1u << 14,
    PF_ON_ICE       = 1u << 15,
    PF_DUAL_MODE    = 1u << 16,
};

struct PhysicsFrame {
    uint64_t frame;
    float    p1_x, p1_y;
    double   p1_y_velocity;
    float    p1_rotation;
    double   p1_platformer_x_velocity;
    float    p1_rotation_speed, p1_gravity, p1_vehicle_size, p1_player_speed;
    double   p1_fall_speed, p1_last_land_time;
    float    p1_slope_rotation;
    uint32_t p1_flags;
    float    p2_x, p2_y;
    double   p2_y_velocity;
    float    p2_rotation;
    double   p2_platformer_x_velocity;
    float    p2_rotation_speed, p2_gravity, p2_vehicle_size, p2_player_speed;
    double   p2_fall_speed, p2_last_land_time;
    float    p2_slope_rotation;
    uint32_t p2_flags;

    // Dash vector (v4) — hold-orb / dash-fire animation fidelity
    double p1_dash_x = 0.0, p1_dash_y = 0.0, p1_dash_angle = 0.0, p1_dash_start_time = 0.0, p1_black_orb_related = 0.0;
    double p2_dash_x = 0.0, p2_dash_y = 0.0, p2_dash_angle = 0.0, p2_dash_start_time = 0.0, p2_black_orb_related = 0.0;

    bool p2Exists() const { return (p2_flags & PF_DUAL_MODE) != 0; }

    bool operator==(const PhysicsFrame& o) const {
        return frame == o.frame &&
               p1_x == o.p1_x && p1_y == o.p1_y && p1_y_velocity == o.p1_y_velocity &&
               p1_rotation == o.p1_rotation && p1_platformer_x_velocity == o.p1_platformer_x_velocity &&
               p1_rotation_speed == o.p1_rotation_speed && p1_gravity == o.p1_gravity &&
               p1_vehicle_size == o.p1_vehicle_size && p1_player_speed == o.p1_player_speed &&
               p1_fall_speed == o.p1_fall_speed && p1_last_land_time == o.p1_last_land_time &&
               p1_slope_rotation == o.p1_slope_rotation && p1_flags == o.p1_flags &&
               p2_x == o.p2_x && p2_y == o.p2_y && p2_y_velocity == o.p2_y_velocity &&
               p2_rotation == o.p2_rotation && p2_platformer_x_velocity == o.p2_platformer_x_velocity &&
               p2_rotation_speed == o.p2_rotation_speed && p2_gravity == o.p2_gravity &&
               p2_vehicle_size == o.p2_vehicle_size && p2_player_speed == o.p2_player_speed &&
               p2_fall_speed == o.p2_fall_speed && p2_last_land_time == o.p2_last_land_time &&
               p2_slope_rotation == o.p2_slope_rotation && p2_flags == o.p2_flags &&
               p1_dash_x == o.p1_dash_x && p1_dash_y == o.p1_dash_y &&
               p1_dash_angle == o.p1_dash_angle && p1_dash_start_time == o.p1_dash_start_time &&
               p1_black_orb_related == o.p1_black_orb_related &&
               p2_dash_x == o.p2_dash_x && p2_dash_y == o.p2_dash_y &&
               p2_dash_angle == o.p2_dash_angle && p2_dash_start_time == o.p2_dash_start_time &&
               p2_black_orb_related == o.p2_black_orb_related;
    }
};

// ── Loop compression (v4 .skk format) ─────────────────────────────────
struct CompressedLoop {
    uint64_t startFrame;
    uint32_t patternLen;
    uint32_t repeatCount;
    uint32_t delayBetweenRepeats = 0;
    uint64_t inputStart;
    uint64_t inputEnd;

    bool operator==(const CompressedLoop& o) const {
        return startFrame == o.startFrame && patternLen == o.patternLen &&
               repeatCount == o.repeatCount && delayBetweenRepeats == o.delayBetweenRepeats &&
               inputStart == o.inputStart && inputEnd == o.inputEnd;
    }
};

// ── Helper: group consecutive inputs at same frame into runs ───────────
inline std::vector<std::pair<size_t, size_t>> buildInputRuns(const std::vector<Input>& inputs) {
    std::vector<std::pair<size_t, size_t>> runs;
    size_t i = 0;
    while (i < inputs.size()) {
        size_t start = i;
        uint64_t f = inputs[i].frame;
        while (i < inputs.size() && inputs[i].frame == f) i++;
        runs.push_back({start, i});
    }
    return runs;
}

inline void decompressInputs(std::vector<Input>& inputs, const std::vector<CompressedLoop>& loops) {
    if (loops.empty()) return;

    auto runs = buildInputRuns(inputs);

    std::vector<bool> isLoopInput(inputs.size(), false);
    for (auto& loop : loops)
        for (uint64_t k = loop.inputStart; k < loop.inputEnd; k++)
            isLoopInput[(size_t)k] = true;

    std::vector<Input> flat;
    for (size_t i = 0; i < runs.size(); ) {
        bool expanded = false;
        for (auto& loop : loops) {
            uint64_t runStartFrame = inputs[runs[i].first].frame;
            if (runStartFrame == loop.startFrame) {
                for (uint32_t r = 0; r < loop.repeatCount; r++) {
                    uint64_t baseFrame = loop.startFrame + (uint64_t)r * loop.patternLen;
                    for (uint64_t k = loop.inputStart; k < loop.inputEnd; k++) {
                        Input in = inputs[(size_t)k];
                        in.frame = baseFrame + (in.frame - loop.startFrame);
                        flat.push_back(in);
                    }
                }
                uint64_t skipRuns = (uint64_t)loop.patternLen * loop.repeatCount;
                uint64_t advanced = 0;
                while (i < runs.size() && advanced < skipRuns) {
                    if (isLoopInput[runs[i].first]) advanced++;
                    i++;
                }
                expanded = true;
                break;
            }
        }
        if (!expanded) {
            for (size_t k = runs[i].first; k < runs[i].second; k++)
                flat.push_back(inputs[k]);
            i++;
        }
    }

    inputs = std::move(flat);
}

struct Macro {
    float    tps = 240.f;
    std::vector<Input> inputs;
    std::vector<PhysicsFrame> physics;
    std::string author, description, level_name;
    int      level_id = 0;
    uint64_t seed = 0;

    // Visual/animation state anchors (sparse, opaque serialized blobs)
    std::vector<std::vector<uint8_t>> visualAnchors;

    // Gameplay loops (drive playback repetition; stored alongside deduped inputs)
    std::vector<CompressedLoop> loops;

    void clear() {
        inputs.clear(); physics.clear();
        author.clear(); description.clear(); level_name.clear();
        level_id = 0; seed = 0; tps = 240.f;
        visualAnchors.clear();
        loops.clear();
    }
};

inline bool loadSkk(const std::string& path, Macro& macro) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    uint32_t magic, version;
    f.read(reinterpret_cast<char*>(&magic), 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    if (magic != SKK_MAGIC || version != SKK_VERSION) return false;

    f.read(reinterpret_cast<char*>(&macro.tps), 4);

    auto readStr = [&](std::string& s) {
        uint32_t len;
        f.read(reinterpret_cast<char*>(&len), 4);
        s.resize(len);
        f.read(s.data(), len);
    };
    readStr(macro.author);
    readStr(macro.description);
    readStr(macro.level_name);
    f.read(reinterpret_cast<char*>(&macro.level_id), 4);
    f.read(reinterpret_cast<char*>(&macro.seed), 8);

    uint64_t icount;
    f.read(reinterpret_cast<char*>(&icount), 8);
    macro.inputs.resize(icount);
    f.read(reinterpret_cast<char*>(macro.inputs.data()), icount * sizeof(Input));

    uint64_t pcount;
    f.read(reinterpret_cast<char*>(&pcount), 8);
    macro.physics.resize(pcount);
    f.read(reinterpret_cast<char*>(macro.physics.data()), pcount * sizeof(PhysicsFrame));

    // Read loop metadata (kept alongside deduped inputs; inputs expanded to flat)
    uint64_t loop_count = 0;
    f.read(reinterpret_cast<char*>(&loop_count), 8);
    std::vector<CompressedLoop> loops(loop_count);
    for (uint64_t i = 0; i < loop_count; i++) {
        f.read(reinterpret_cast<char*>(&loops[(size_t)i].startFrame), 8);
        f.read(reinterpret_cast<char*>(&loops[(size_t)i].patternLen), 4);
        f.read(reinterpret_cast<char*>(&loops[(size_t)i].repeatCount), 4);
        f.read(reinterpret_cast<char*>(&loops[(size_t)i].delayBetweenRepeats), 4);
        uint64_t inStart, inEnd;
        f.read(reinterpret_cast<char*>(&inStart), 8);
        f.read(reinterpret_cast<char*>(&inEnd), 8);
        loops[(size_t)i].inputStart = inStart;
        loops[(size_t)i].inputEnd = inEnd;
    }
    macro.loops = loops;

    // Expand deduped inputs → flat (loop metadata is preserved separately)
    if (!loops.empty())
        decompressInputs(macro.inputs, loops);

    // Read visual anchor blobs (always present in v5)
    macro.visualAnchors.clear();
    {
        uint64_t va_count = 0;
        f.read(reinterpret_cast<char*>(&va_count), 8);
        macro.visualAnchors.resize((size_t)va_count);
        for (uint64_t i = 0; i < va_count; i++) {
            uint64_t blen = 0;
            f.read(reinterpret_cast<char*>(&blen), 8);
            auto& blob = macro.visualAnchors[(size_t)i];
            blob.resize((size_t)blen);
            if (blen) f.read(reinterpret_cast<char*>(blob.data()), (size_t)blen);
        }
    }

    return true;
}

inline bool saveSkk(const std::string& path, const Macro& macro) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    uint32_t magic = SKK_MAGIC, version = SKK_VERSION;
    f.write(reinterpret_cast<const char*>(&magic), 4);
    f.write(reinterpret_cast<const char*>(&version), 4);
    f.write(reinterpret_cast<const char*>(&macro.tps), 4);

    auto writeStr = [&](const std::string& s) {
        uint32_t len = (uint32_t)s.size();
        f.write(reinterpret_cast<const char*>(&len), 4);
        f.write(s.data(), len);
    };
    writeStr(macro.author);
    writeStr(macro.description);
    writeStr(macro.level_name);
    f.write(reinterpret_cast<const char*>(&macro.level_id), 4);
    f.write(reinterpret_cast<const char*>(&macro.seed), 8);

    uint64_t icount = macro.inputs.size();
    f.write(reinterpret_cast<const char*>(&icount), 8);
    f.write(reinterpret_cast<const char*>(macro.inputs.data()), icount * sizeof(Input));

    uint64_t pcount = macro.physics.size();
    f.write(reinterpret_cast<const char*>(&pcount), 8);
    f.write(reinterpret_cast<const char*>(macro.physics.data()), pcount * sizeof(PhysicsFrame));

    // Write loop metadata (v6 layout: + delayBetweenRepeats)
    uint64_t lcount = macro.loops.size();
    f.write(reinterpret_cast<const char*>(&lcount), 8);
    for (auto& loop : macro.loops) {
        f.write(reinterpret_cast<const char*>(&loop.startFrame), 8);
        f.write(reinterpret_cast<const char*>(&loop.patternLen), 4);
        f.write(reinterpret_cast<const char*>(&loop.repeatCount), 4);
        f.write(reinterpret_cast<const char*>(&loop.delayBetweenRepeats), 4);
        uint64_t inStart = loop.inputStart, inEnd = loop.inputEnd;
        f.write(reinterpret_cast<const char*>(&inStart), 8);
        f.write(reinterpret_cast<const char*>(&inEnd), 8);
    }

    // Write visual anchor blobs (v5)
    uint64_t va_count = macro.visualAnchors.size();
    f.write(reinterpret_cast<const char*>(&va_count), 8);
    for (auto& blob : macro.visualAnchors) {
        uint64_t blen = blob.size();
        f.write(reinterpret_cast<const char*>(&blen), 8);
        if (blen) f.write(reinterpret_cast<const char*>(blob.data()), (size_t)blen);
    }

    return true;
}

} // namespace skc
