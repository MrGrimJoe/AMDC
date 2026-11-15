// AMDC-F: fast-decode mode. LZ77 block codec in the LZ4 block format (so any LZ4 decoder can read it),
// plus a page codec for RAM compression and a multi-block, multi-thread container.
// Little-endian targets only (x86-64, ARM64: every Windows target).
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace amdc1 {
namespace fast {

using Bytes = std::vector<uint8_t>;

struct FastError : std::runtime_error {
    explicit FastError(const char* m) : std::runtime_error(m) {}
};

// ---- raw block codec (LZ4 block format; max block = any size, window 64 KiB) ----------------------
inline size_t block_bound(size_t n) { return n + n / 255 + 32; }

// level 1: fastest compressor. level 2: hash chains + lazy matching (smaller output, same decode speed).
// `dst` must hold block_bound(n) bytes. Returns the compressed size.
size_t block_compress(const uint8_t* src, size_t n, uint8_t* dst, int level = 1);

// Memory-safe on arbitrary input. Returns true iff `src` decodes to exactly `out_len` bytes.
bool block_decompress(const uint8_t* src, size_t n, uint8_t* dst, size_t out_len);

// ---- page format v2: same LZ4-style tokens but 1-byte near/repeat offsets. NOT LZ4 compatible. ---------
constexpr size_t kPage2Max = 4336;   // largest input it can address (offsets are at most 240 + 4095)
size_t page2_compress(const uint8_t* src, size_t n, uint8_t* dst, int level = 1);   // dst >= block_bound(n)
bool page2_decompress(const uint8_t* src, size_t n, uint8_t* dst, size_t out_len);

// Complete page codec on top of page2: zero / fill pages, LZ2, and (level >= 3) LZ2 with Huffman literals when that
// is smaller. level 1 = fast parse, 2 = chained parse, 3 = fast parse + Huffman literals, 4 = chained + Huffman.
// Returns the blob size (first byte = kind), or 0 when the page does not shrink. Pages up to kPage2Max bytes only.
// `out` needs page3_bound(n) bytes.
inline size_t page3_bound(size_t n) { return block_bound(n) + 16; }
size_t page3_compress(const uint8_t* page, size_t n, uint8_t* out, int level = 1);
bool page3_decompress(const uint8_t* enc, size_t len, uint8_t* page, size_t n);

// ---- page codec (RAM compression building block) --------------------------------------------------
enum PageKind : uint8_t { PAGE_RAW = 0, PAGE_ZERO = 1, PAGE_FILL = 2, PAGE_LZ = 3, PAGE_LZ2 = 4, PAGE_HUF = 5 };

inline size_t page_encode_bound(size_t page) { return block_bound(page) + 17; }

// Levels: 1 and 2 write LZ4-format blobs (kind PAGE_LZ). Levels 3-7 (pages up to kPage2Max bytes; larger pages fall
// back to levels 1/2) write the page v2/v3 formats: 3 = fast parse, 4 = chained parse, 5 = fast parse + Huffman
// literals, 6 = chained parse + Huffman literals, 7 = chained parse + Huffman literals AND Huffman sequence bytes.
// Level 5 is the best speed/size balance; level 7 is smallest (slowest to compress, ~30% slower to decode than 6).
// Encodes one page into `out` (>= page_encode_bound(page) bytes). Returns the encoded size, or 0 when the
// page is not worth compressing (encoded size would exceed page * max_pct / 100): keep it uncompressed.
// All-zero and single-byte-fill pages cost 1 and 2 bytes.
size_t page_compress(const uint8_t* page, size_t page_size, uint8_t* out, int level = 1, unsigned max_pct = 75);
bool page_decompress(const uint8_t* enc, size_t enc_len, uint8_t* page, size_t page_size);

// ---- container ("AMDF"): independent blocks, parallel encode and decode ---------------------------
constexpr size_t kDefaultBlock = size_t(256) << 10;
Bytes compress(const Bytes& in, size_t block_size = kDefaultBlock, int level = 1, unsigned threads = 0,
               bool checksum = true);
Bytes decompress(const Bytes& in, uint64_t max_output = uint64_t(1) << 32, unsigned threads = 0);

// Allocation-free decode into a caller buffer (what a RAM/page decompressor wants).
// Throws FastError if the archive is malformed or `cap` is smaller than the decoded size.
size_t decompressed_size(const uint8_t* in, size_t inlen);
size_t decompress_into(const uint8_t* in, size_t inlen, uint8_t* dst, size_t cap, unsigned threads = 1);

uint64_t hash64(const uint8_t* p, size_t n);

}  // namespace fast
}  // namespace amdc1
