// AMDC-B: BWT + move-to-front + context-mixed binary coding of the ranks. Middle tier between AMDC-F (LZ, GB/s)
// and AMDC-1 (context mixing, ~0.5 MB/s): several MB/s per core with ratios better than xz on most data.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace amdc1 {
namespace bwtc {

using Bytes = std::vector<uint8_t>;
constexpr size_t kDefaultBlock = size_t(8) << 20;

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Bytes compress(const Bytes& in, size_t block_size = kDefaultBlock, unsigned threads = 0);
Bytes decompress(const Bytes& in, uint64_t max_output = uint64_t(1) << 32, unsigned threads = 0);

// Raw rank coder (exposed for tests): encodes `n` bytes (already BWT-transformed) / decodes them.
Bytes rank_encode(const uint8_t* L, size_t n);
Bytes rank_decode(const uint8_t* payload, size_t plen, size_t n);

}  // namespace bwtc
}  // namespace amdc1
