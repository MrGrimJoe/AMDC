// AMDC-1 v1.2 (format version 2) -- adaptive multi-model lossless compression (C++17 reference implementation)
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace amdc1 {

using Bytes = std::vector<uint8_t>;

struct FormatError : std::runtime_error {
    explicit FormatError(const char* m) : std::runtime_error(m) {}
};

constexpr size_t kDefaultBlock = size_t(1) << 20;

// Raw context-mixing codec. `s` = context stride (0 = text-style contexts).
Bytes cm_encode(const uint8_t* data, size_t n, uint32_t s);
Bytes cm_decode(const uint8_t* payload, size_t plen, size_t n, uint32_t s);

// level: 0 fixed model, 1 sampled transform search, 2 exhaustive search.
// threads: 0 = hardware concurrency. Output is identical for any thread count.
Bytes compress(const Bytes& in, size_t block_size = kDefaultBlock, int level = 1, unsigned threads = 0);
Bytes decompress(const Bytes& in, uint64_t max_output = uint64_t(1) << 32, unsigned threads = 0);

uint32_t crc32(const uint8_t* p, size_t n);

}  // namespace amdc1
