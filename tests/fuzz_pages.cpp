// libFuzzer harness for the AMDC-F page and block codecs.
//   clang++ -std=c++17 -O1 -g -fsanitize=fuzzer,address,undefined -pthread src/fast.cpp tests/fuzz_pages.cpp -o build/fuzz_pages
//   ./build/fuzz_pages -max_total_time=60 -max_len=8192
// Input layout: byte0 = mode/level, byte1 = page-size selector, rest = payload.
//   decode mode: the payload is an arbitrary "compressed" blob and must never crash or read/write out of bounds;
//   round-trip mode: the payload is a page, compress then decompress must give the page back.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/fast.hpp"
using namespace amdc1::fast;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 3) return 0;
    const unsigned mode = data[0], sel = data[1];
    data += 2; size -= 2;
    static const size_t sizes[] = {64, 256, 1024, 4096, 4096, 4096, 4336, 8192};
    const size_t page = sizes[sel & 7];
    std::vector<uint8_t> out(page + 64, 0xAB);
    if (mode & 1) {   // decode arbitrary bytes
        page_decompress(data, size, out.data(), page);
        block_decompress(data, size, out.data(), page);
        page3_decompress(data, size, out.data(), page);
        for (size_t i = page; i < out.size(); ++i) if (out[i] != 0xAB) abort();   // wrote past the page
    } else {          // round trip
        std::vector<uint8_t> pg(page, 0);
        std::memcpy(pg.data(), data, size < page ? size : page);
        if (sel & 8) for (size_t i = size; i < page; ++i) pg[i] = pg[i % (size ? size : 1)];   // repeat the payload: gives matches
        const int level = 1 + int((mode >> 1) % 8);
        std::vector<uint8_t> enc(page_encode_bound(page) + 64);
        const size_t n = page_compress(pg.data(), page, enc.data(), level);
        if (n) {
            if (!page_decompress(enc.data(), n, out.data(), page)) abort();
            if (std::memcmp(out.data(), pg.data(), page) != 0) abort();
        }
    }
    return 0;
}
