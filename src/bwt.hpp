// Burrows-Wheeler transform (SA-IS suffix sorting) with a multi-chain inverse for memory-level parallelism.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace amdc1 {
namespace bwt {

constexpr int kChains = 4;                          // interleaved inverse walks
constexpr size_t kMaxBlock = (size_t(1) << 24) - 64;  // row index + symbol are packed into 32 bits

struct Header {
    uint32_t primary = 0;                    // row whose last-column symbol is the end marker
    uint32_t starts[kChains - 1] = {};       // rows where chains 1..kChains-1 begin (chain 0 begins at row 0)
};

// L receives n bytes (the last column without the end marker). n must be 1..kMaxBlock.
void forward(const uint8_t* in, size_t n, uint8_t* L, Header& h);
// Returns false if the header is inconsistent with n.
bool inverse(const uint8_t* L, size_t n, const Header& h, uint8_t* out);

}  // namespace bwt
}  // namespace amdc1
