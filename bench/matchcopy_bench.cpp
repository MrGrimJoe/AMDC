// AMDC-F bounded-match-copy benchmark. SYNTHETIC: one hot block decoded repeatedly from L1/L2 cache, so the
// GB/s here is the decoder's best case for long short-period matches, not real-data throughput.
// Build once against the old library and once against the patched library,
// using the same compiler flags and machine. This focuses on valid LZ4 blocks
// containing a long short-period match followed by a final literal run (five bytes by default).
#include "../src/fast.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using amdc1::fast::Bytes;
using amdc1::fast::block_decompress;

static void append_len(Bytes& out, size_t n) {
    while (n >= 255) { out.push_back(255); n -= 255; }
    out.push_back(uint8_t(n));
}

static Bytes make_block(size_t offset, size_t match_len, size_t tail) {
    const size_t ml_code = match_len - 4;
    Bytes in;
    in.push_back(uint8_t((std::min<size_t>(15, offset) << 4) | std::min<size_t>(15, ml_code)));
    if (offset >= 15) append_len(in, offset - 15);
    for (size_t i = 0; i < offset; ++i) in.push_back(uint8_t('A' + i));
    in.push_back(uint8_t(offset));
    in.push_back(uint8_t(offset >> 8));
    if (ml_code >= 15) append_len(in, ml_code - 15);
    if (tail < 15) in.push_back(uint8_t(tail << 4));
    else {
        in.push_back(0xF0);
        append_len(in, tail - 15);
    }
    for (size_t i = 0; i < tail; ++i) in.push_back(uint8_t('x' + (i % 8)));
    return in;
}

int main(int argc, char** argv) {
    const size_t match_len = argc > 1 ? size_t(std::strtoull(argv[1], nullptr, 10)) : 4096;
    const size_t target_bytes = argc > 2 ? size_t(std::strtoull(argv[2], nullptr, 10)) : (size_t(256) << 20);
    const size_t tail = argc > 3 ? size_t(std::strtoull(argv[3], nullptr, 10)) : 5;
    if (match_len < 128 || match_len > (size_t(1) << 26) || tail < 5 || tail > (size_t(1) << 20)) {
        std::fprintf(stderr, "match_len must be in [128, 67108864], tail in [5, 1048576]\n");
        return 2;
    }
    for (size_t offset : {size_t(2), size_t(3), size_t(4), size_t(8), size_t(15), size_t(16), size_t(17), size_t(32), size_t(64)}) {
        Bytes in = make_block(offset, match_len, tail);
        Bytes out(offset + match_len + tail);
        if (!block_decompress(in.data(), in.size(), out.data(), out.size())) {
            std::fprintf(stderr, "decoder rejected generated test block\n");
            return 1;
        }
        const size_t reps = std::max<size_t>(1000, target_bytes / out.size());
        volatile uint64_t checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < reps; ++i) {
            if (!block_decompress(in.data(), in.size(), out.data(), out.size())) return 1;
            checksum = checksum + out[(i * 131) % out.size()];
        }
        const auto stop = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(stop - start).count();
        const double gbps = double(reps) * double(out.size()) / seconds / 1e9;
        std::printf("offset=%2zu match=%8zu tail=%5zu  %.2f GB/s  (checksum=%llu)\n",
                    offset, match_len, tail, gbps, (unsigned long long)checksum);
    }
}
