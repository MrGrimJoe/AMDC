// AMDC-1 v1.3 (format version 3) -- adaptive multi-model lossless compression (C++17 reference implementation)
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

constexpr size_t kDefaultBlock = size_t(16) << 20;   // larger blocks share more context; memory use does not grow with block size
constexpr int kDefaultMemBits = 22;                  // log2 slots per large model table (4 MiB slots = 16 MiB each)
constexpr int kMinMemBits = 16, kMaxMemBits = 24;

// Model configuration stored in every compressed block.
//   lite    : 0 = full model (12 context models, 2 match models, 3 mixers + 2nd layer, 2 APMs)
//             1 = lite model (6 context models, 1 match model, 2 mixers, 1 APM): about 2x faster, a few percent larger
//   membits : log2 of slots per large context table; bounds memory (about 10 tables x 4 B x 2^membits full, 5 lite)
struct CmCfg { uint8_t lite = 0; uint8_t membits = kDefaultMemBits; };

// Raw context-mixing codec. `s` = context stride (0 = text-style contexts).
Bytes cm_encode(const uint8_t* data, size_t n, uint32_t s, const CmCfg& cfg = CmCfg());
Bytes cm_decode(const uint8_t* payload, size_t plen, size_t n, uint32_t s, const CmCfg& cfg = CmCfg());

// level: 0 lite model, fixed settings; 1 full model + sampled transform search; 2 full model + exhaustive search.
// threads: 0 = hardware concurrency. Output is identical for any thread count.
Bytes compress(const Bytes& in, size_t block_size = kDefaultBlock, int level = 1, unsigned threads = 0,
               int membits = kDefaultMemBits);
Bytes decompress(const Bytes& in, uint64_t max_output = uint64_t(1) << 32, unsigned threads = 0);

uint32_t crc32(const uint8_t* p, size_t n);

}  // namespace amdc1
