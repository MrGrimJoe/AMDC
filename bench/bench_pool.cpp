// PagePool throughput: compress and decompress a page batch at several thread counts and levels.
// build: g++ -std=c++17 -O3 -march=native -pthread src/fast.cpp bench/bench_pool.cpp -o build/bench_pool
// usage: bench_pool [-r REPS] [-t MAXTHREADS] [-l LEVELS e.g. 1,2,5,8] FILE...
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/fast.hpp"
using namespace amdc1::fast;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    int reps = 5;
    unsigned maxt = 0;
    std::vector<int> levels = {1, 2, 5, 8};
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-r" && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (a == "-t" && i + 1 < argc) maxt = unsigned(std::atoi(argv[++i]));
        else if (a == "-l" && i + 1 < argc) { levels.clear(); std::stringstream ss(argv[++i]); std::string x; while (std::getline(ss, x, ',')) levels.push_back(std::atoi(x.c_str())); }
        else files.push_back(a);
    }
    if (files.empty()) { std::fprintf(stderr, "usage: bench_pool [-r REPS] [-t MAXTHREADS] [-l 1,2,5,8] FILE...\n"); return 64; }
    if (!maxt) maxt = std::max(1u, std::thread::hardware_concurrency());
    const size_t P = 4096;
    std::vector<uint8_t> mem;
    for (auto& f : files) { std::ifstream in(f, std::ios::binary); std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {}); mem.insert(mem.end(), b.begin(), b.begin() + long(b.size() / P * P)); }
    const size_t np = mem.size() / P, stride = page_encode_bound(P);
    std::printf("%zu pages (%.0f MB), pool threads 1..%u\n%-8s %-8s %8s %12s %12s\n", np, double(mem.size()) / 1e6, maxt, "level", "threads", "size%", "comp GB/s", "decomp GB/s");
    std::vector<uint8_t> enc(np * stride), out(np * P);
    std::vector<size_t> len(np);
    for (int level : levels)
        for (unsigned th = 1; th <= maxt; th = th < 4 ? th + 1 : th * 2) {
            PagePool pool(th);
            double bc = 1e9, bd = 1e9;
            for (int r = 0; r < reps; ++r) {
                auto t = Clock::now();
                pool.compress(mem.data(), P, np, enc.data(), stride, len.data(), level);
                bc = std::min(bc, std::chrono::duration<double>(Clock::now() - t).count());
            }
            std::vector<PageRef> refs;
            std::vector<size_t> idx;
            double comp = 0;
            for (size_t i = 0; i < np; ++i) {
                comp += len[i] ? double(len[i]) : double(P);
                if (len[i]) { refs.push_back(PageRef{&enc[i * stride], len[i]}); idx.push_back(i); }
            }
            double raw_decoded = double(refs.size()) * P;
            for (int r = 0; r < reps; ++r) {
                auto t = Clock::now();
                size_t bad = pool.decompress(refs.data(), refs.size(), out.data(), P);
                bd = std::min(bd, std::chrono::duration<double>(Clock::now() - t).count());
                if (bad) { std::fprintf(stderr, "DECODE FAIL\n"); return 2; }
            }
            for (size_t k = 0; k < idx.size(); ++k)
                if (std::memcmp(&out[k * P], &mem[idx[k] * P], P)) { std::fprintf(stderr, "MISMATCH page %zu\n", idx[k]); return 2; }
            std::printf("%-8d %-8u %7.2f%% %12.2f %12.2f\n", level, th, 100.0 * comp / double(mem.size()), double(mem.size()) / 1e9 / bc, raw_decoded / 1e9 / bd);
        }
    return 0;
}
