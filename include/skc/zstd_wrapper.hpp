#pragma once
#include <cstdint>
#include <vector>

std::vector<uint8_t> zstd_compress(const std::vector<uint8_t>& input, int level);
std::vector<uint8_t> zstd_decompress(const std::vector<uint8_t>& input);
