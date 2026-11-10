#pragma once
#include "amdc1.hpp"

namespace amdc1 {
enum Xf : uint8_t { XF_ID = 0, XF_DELTA = 1, XF_XOR = 2, XF_SHUFFLE = 3, XF_IMAGE = 4 };

// Length-preserving reversible transforms (spec section 2).
Bytes xf_apply(int xf, uint32_t k, const uint8_t* d, size_t n);
Bytes xf_invert(int xf, uint32_t k, const uint8_t* d, size_t n);

struct PnmInfo { size_t hdr; uint32_t w, h, ch; };
bool parse_pnm(const uint8_t* d, size_t n, PnmInfo& out);
Bytes image_fwd(const uint8_t* d, size_t n, const PnmInfo& pi, int ct);
Bytes image_inv(const uint8_t* y, size_t n, const PnmInfo& pi, int ct);
}  // namespace amdc1
