#include "skc/format.hpp"
#include "skc/codec.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

static bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize((size_t)(n > 0 ? n : 0));
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    return true;
}

static bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    if (!data.empty())
        f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return true;
}

static void printMacro(const char* label, const skc::Macro& m) {
    size_t anchorBytes = 0;
    for (auto& b : m.visualAnchors) anchorBytes += b.size();
    printf("%s: tps=%.1f inputs=%zu physics=%zu visualAnchors=%zu (%.1f KiB)\n",
           label, m.tps, m.inputs.size(), m.physics.size(),
           m.visualAnchors.size(), anchorBytes / 1024.0);
}

static int cmdCompress(const std::string& in, const std::string& out) {
    skc::Macro m;
    if (!skc::loadSkk(in, m)) { printf("error: cannot load .skk '%s'\n", in.c_str()); return 1; }
    skc::SKCCompressResult r = skc::skc_compress_v4(m);
    if (r.data.empty()) { printf("error: compression failed\n"); return 1; }
    if (!writeFile(out, r.data)) { printf("error: cannot write '%s'\n", out.c_str()); return 1; }
    printMacro("source", m);
    printf("wrote %s (%.2f%% of raw, %zu bytes)\n", out.c_str(), r.compression_ratio * 100.0, r.data.size());
    return 0;
}

static int cmdDecompress(const std::string& in, const std::string& out) {
    std::vector<uint8_t> data;
    if (!readFile(in, data)) { printf("error: cannot read '%s'\n", in.c_str()); return 1; }
    skc::Macro m;
    if (!skc::skc_decompress(data, m)) { printf("error: not a valid .skc\n"); return 1; }
    if (!skc::saveSkk(out, m)) { printf("error: cannot write '%s'\n", out.c_str()); return 1; }
    printMacro("decoded", m);
    printf("wrote %s\n", out.c_str());
    return 0;
}

static int cmdInfo(const std::string& in) {
    std::vector<uint8_t> data;
    if (!readFile(in, data)) { printf("error: cannot read '%s'\n", in.c_str()); return 1; }
    if (data.size() >= 4 && memcmp(data.data(), "SKC3", 4) == 0) {
        skc::Macro m;
        if (!skc::skc_decompress(data, m)) { printf("error: invalid .skc body\n"); return 1; }
        printf("format: .skc (SKC3 v5)\n");
        printMacro("macro", m);
    } else if (data.size() >= 4 && memcmp(data.data(), "SKKB", 4) == 0) {
        skc::Macro m;
        if (!skc::loadSkk(in, m)) { printf("error: invalid .skk\n"); return 1; }
        printf("format: .skk (SKKB v%d)\n", skc::SKK_VERSION);
        printMacro("macro", m);
    } else {
        printf("error: unknown magic\n");
        return 1;
    }
    return 0;
}

static int cmdVerify(const std::string& in) {
    std::vector<uint8_t> data;
    if (!readFile(in, data)) { printf("error: cannot read '%s'\n", in.c_str()); return 1; }
    skc::Macro m;
    if (!skc::skc_decompress(data, m)) { printf("error: not a valid .skc\n"); return 1; }
    printMacro("decoded", m);
    skc::SKCCompressResult r = skc::skc_compress_v4(m);
    skc::Macro m2;
    if (!skc::skc_decompress(r.data, m2)) { printf("error: re-decode failed\n"); return 1; }
    bool ok = m.physics.size() == m2.physics.size()
           && m.inputs.size() == m2.inputs.size()
           && m.visualAnchors.size() == m2.visualAnchors.size()
           && m.physics == m2.physics
           && m.inputs == m2.inputs
           && m.visualAnchors == m2.visualAnchors;
    printf("round-trip: %s\n", ok ? "OK (lossless)" : "MISMATCH");
    return ok ? 0 : 1;
}

static void usage() {
    printf("skcconv - .skk / .skc converter (skc library)\n");
    printf("usage:\n");
    printf("  skcconv compress   <in.skk> <out.skc>\n");
    printf("  skcconv decompress <in.skc> <out.skk>\n");
    printf("  skcconv info       <in.skc|in.skk>\n");
    printf("  skcconv verify     <in.skc>\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    if (cmd == "compress"   && argc == 4) return cmdCompress(argv[2], argv[3]);
    if (cmd == "decompress" && argc == 4) return cmdDecompress(argv[2], argv[3]);
    if (cmd == "info"       && argc == 3) return cmdInfo(argv[2]);
    if (cmd == "verify"     && argc == 3) return cmdVerify(argv[2]);
    usage();
    return 1;
}
