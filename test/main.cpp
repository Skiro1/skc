#include "skc/format.hpp"
#include "skc/codec.hpp"

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <random>
#include <fstream>

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); g_fail++; } } while (0)

static bool readFile(const std::string& p, std::vector<uint8_t>& o) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    o.resize((size_t)(n > 0 ? n : 0));
    if (n > 0) f.read(reinterpret_cast<char*>(o.data()), n);
    return true;
}

static skc::Macro synth(size_t n) {
    skc::Macro m;
    m.tps = 240.f; m.author = "t"; m.level_name = "lv"; m.level_id = 7; m.seed = 99;
    std::mt19937_64 rng(123);
    for (size_t i = 0; i < n; i++) {
        skc::PhysicsFrame pf{};
        pf.frame = i;
        pf.p1_x = (float)(rng() % 1000) / 9.f;
        pf.p1_y = (float)(rng() % 1000) / 9.f;
        pf.p1_y_velocity = (double)(rng() % 1000) / 3.0;
        pf.p1_rotation = (float)(rng() % 360);
        pf.p1_flags = (uint32_t)(rng() & 0xFFFF);
        pf.p1_gravity = (float)(rng() % 10);
        m.physics.push_back(pf);
    }
    for (size_t i = 0; i < n / 4; i++) {
        skc::Input in;
        in.frame = i * 4;
        in.button = (skc::Button)(1 + (rng() % 3));
        in.player2 = (rng() & 1);
        in.down = (rng() & 1);
        m.inputs.push_back(in);
    }
    for (size_t i = 0; i < (n / 30) + 2; i++) {
        std::vector<uint8_t> b;
        for (size_t k = 0; k < 12; k++) b.push_back((uint8_t)(rng() & 0xFF));
        m.visualAnchors.push_back(b);
    }

    // Add a gameplay loop referencing the first two inputs (frames 0 and 4)
    if (m.inputs.size() >= 2) {
        skc::CompressedLoop lp;
        lp.startFrame = m.inputs[0].frame;
        lp.patternLen = (uint32_t)(m.inputs[1].frame - m.inputs[0].frame);
        lp.repeatCount = 3;
        lp.delayBetweenRepeats = 0;
        lp.inputStart = 0;
        lp.inputEnd = 2;
        m.loops.push_back(lp);
    }
    return m;
}

static bool same(const skc::Macro& a, const skc::Macro& b) {
    if (a.physics.size() != b.physics.size() ||
        a.inputs.size()  != b.inputs.size()  ||
        a.visualAnchors.size() != b.visualAnchors.size() ||
        a.loops.size() != b.loops.size()) return false;
    if (a.physics != b.physics || a.inputs != b.inputs) return false;
    if (a.loops != b.loops) return false;
    for (size_t i = 0; i < a.visualAnchors.size(); i++)
        if (a.visualAnchors[i] != b.visualAnchors[i]) return false;
    return true;
}

int main(int argc, char** argv) {
    // 1) Synthetic round-trip
    skc::Macro orig = synth(3000);
    skc::SKCCompressResult cr = skc::skc_compress_v4(orig);
    CHECK(!cr.data.empty(), "compress produced bytes");
    skc::Macro dec;
    CHECK(skc::skc_decompress(cr.data, dec), "decompress success");
    CHECK(same(orig, dec), "round-trip lossless");
    printf("round-trip: %zu phys, %zu inputs, %zu anchors, %zu loops, %.1f%% size\n",
           orig.physics.size(), orig.inputs.size(), orig.visualAnchors.size(),
           orig.loops.size(), cr.compression_ratio * 100.0);

    // 2) Real bot-produced .skc (Rolling Ball)
    std::string path = (argc > 1) ? argv[1]
        : "C:\\Users\\User\\AppData\\Local\\GeometryDash\\geode\\mods\\skiro1.skk-bot\\macros\\Rolling Ball.skc";
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) {
        printf("WARN: cannot read real file '%s' (skipping)\n", path.c_str());
    } else {
        skc::Macro rb;
        CHECK(skc::skc_decompress(bytes, rb), "decode Rolling Ball.skc");
        printf("Rolling Ball.skc: %zu phys, %zu inputs, %zu anchors, %zu loops\n",
               rb.physics.size(), rb.inputs.size(), rb.visualAnchors.size(),
               rb.loops.size());
        CHECK(rb.physics.size() > 0, "non-empty physics");
        CHECK(rb.visualAnchors.size() > 0, "has visual anchors");
    }

    if (g_fail == 0) printf("ALL TESTS PASSED\n");
    else printf("%d TEST(S) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
