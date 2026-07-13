#include "skc/zstd_wrapper.hpp"
#include <Windows.h>
#include <cstring>

// Dynamic load zstd.dll — avoids MinGW/MSVC ABI mismatch
struct ZstdDll {
    HMODULE mod = nullptr;
    size_t (*compressBound)(size_t) = nullptr;
    size_t (*compress)(void*, size_t, const void*, size_t, int) = nullptr;
    unsigned long long (*getFrameContentSize)(const void*, size_t) = nullptr;
    size_t (*decompress)(void*, size_t, const void*, size_t) = nullptr;

    bool load() {
        mod = LoadLibraryA("libzstd.dll");
        if (!mod) return false;
        compressBound = (decltype(compressBound))GetProcAddress(mod, "ZSTD_compressBound");
        compress      = (decltype(compress))GetProcAddress(mod, "ZSTD_compress");
        getFrameContentSize = (decltype(getFrameContentSize))GetProcAddress(mod, "ZSTD_getFrameContentSize");
        decompress    = (decltype(decompress))GetProcAddress(mod, "ZSTD_decompress");
        return compressBound && compress && getFrameContentSize && decompress;
    }
    ~ZstdDll() { if (mod) FreeLibrary(mod); }
};

static ZstdDll& getZstd() {
    static ZstdDll z;
    static bool loaded = z.load();
    (void)loaded;
    return z;
}

std::vector<uint8_t> zstd_compress(const std::vector<uint8_t>& input, int level) {
    auto& z = getZstd();
    if (!z.mod || input.empty()) return {};

    size_t bound = z.compressBound(input.size());
    std::vector<uint8_t> out(bound);

    size_t ret = z.compress(out.data(), bound, input.data(), input.size(), level);
    if (ret == 0) return {};

    out.resize(ret);
    return out;
}

std::vector<uint8_t> zstd_decompress(const std::vector<uint8_t>& input) {
    auto& z = getZstd();
    if (!z.mod || input.empty()) return {};

    unsigned long long dlen = z.getFrameContentSize(input.data(), input.size());
    if (dlen == 0 || dlen == (unsigned long long)-1) return {};

    std::vector<uint8_t> out((size_t)dlen);
    size_t ret = z.decompress(out.data(), out.size(), input.data(), input.size());
    if (ret == 0) return {};

    return out;
}
