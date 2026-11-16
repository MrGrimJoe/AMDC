// Standalone driver for tests/fuzz_pages.cpp for toolchains without libFuzzer (gcc, MSVC):
//   g++ -std=c++17 -O1 -g -fsanitize=address,undefined -pthread src/fast.cpp tests/fuzz_pages.cpp tests/fuzz_driver.cpp -o build/fuzz_driver
//   ./build/fuzz_driver SECONDS [SEED]
// Random and mutated inputs (bit flips, splices of real compressed blobs); any crash, sanitizer report or abort() fails.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "../src/fast.hpp"
extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t);
using namespace amdc1::fast;

int main(int argc, char** argv) {
    const double secs = argc > 1 ? std::atof(argv[1]) : 10;
    std::mt19937_64 rng(argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1);
    std::vector<std::vector<uint8_t>> seeds;   // real blobs from every level, as mutation fodder
    for (int shape = 0; shape < 6; ++shape) {
        std::vector<uint8_t> pg(4096);
        for (size_t i = 0; i < pg.size(); ++i) {
            switch (shape) {
                case 0: pg[i] = uint8_t(i / 7); break;
                case 1: pg[i] = uint8_t("the quick brown fox "[i % 20]); break;
                case 2: pg[i] = uint8_t(rng() % 4); break;
                case 3: pg[i] = (i % 8 < 3) ? uint8_t(i >> 3) : 0; break;
                case 4: pg[i] = uint8_t(rng() & ((i % 64 < 32) ? 255 : 3)); break;
                default: pg[i] = uint8_t(((i * 2654435761u) >> 13) & 15); break;
            }
        }
        for (int level = 1; level <= 8; ++level) {
            std::vector<uint8_t> enc(page_encode_bound(4096));
            const size_t n = page_compress(pg.data(), 4096, enc.data(), level);
            if (n) { enc.resize(n); seeds.push_back(enc); }
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    size_t runs = 0;
    std::vector<uint8_t> buf;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        buf.clear();
        const unsigned kind = unsigned(rng() % 4);
        if (kind == 0) {                                   // pure random
            buf.resize(3 + rng() % 4200);
            for (auto& b : buf) b = uint8_t(rng());
            buf[0] |= 1;
        } else if (kind == 1) {                            // random round-trip input
            buf.resize(3 + rng() % 5000);
            for (auto& b : buf) b = uint8_t(rng() % (1 + rng() % 256));
            buf[0] &= ~1;
        } else {                                           // mutated real blob (decode mode)
            const auto& s = seeds[rng() % seeds.size()];
            buf.push_back(1);
            buf.push_back(uint8_t(rng() % 8 < 6 ? 3 : rng()));   // mostly 4096-byte pages
            buf.insert(buf.end(), s.begin(), s.end());
            const unsigned flips = 1 + unsigned(rng() % 6);
            for (unsigned f = 0; f < flips; ++f) {
                const size_t at = 2 + rng() % (buf.size() - 2);
                switch (rng() % 3) {
                    case 0: buf[at] ^= uint8_t(1u << (rng() % 8)); break;
                    case 1: buf[at] = uint8_t(rng()); break;
                    default: if (buf.size() > 8) buf.erase(buf.begin() + long(at)); break;
                }
            }
            if (kind == 3 && buf.size() > 12) buf.resize(2 + rng() % (buf.size() - 2));   // truncation
        }
        LLVMFuzzerTestOneInput(buf.data(), buf.size());
        ++runs;
    }
    std::printf("fuzz_driver: %zu inputs, no failures\n", runs);
    return 0;
}
