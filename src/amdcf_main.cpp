// amdcf: fast-decode compressor CLI + in-memory benchmarks + RAM-compression simulator.
#include <algorithm>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>

#include "fast.hpp"
#include "pagecache.hpp"

using namespace amdc1::fast;
using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

static Bytes read_all(const std::string& path) {
    std::istream* is = &std::cin;
    std::ifstream f;
    if (path != "-") {
        f.open(path, std::ios::binary);
        if (!f) throw std::runtime_error("cannot open " + path);
        is = &f;
    }
    return Bytes(std::istreambuf_iterator<char>(*is), std::istreambuf_iterator<char>());
}
static void write_all(const std::string& path, const Bytes& d) {
    if (path == "-") { std::fwrite(d.data(), 1, d.size(), stdout); return; }
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}

static int usage() {
    std::fputs(
        "amdcf - AMDC-F fast-decode compressor (LZ4-format blocks, parallel container)\n"
        "  amdcf compress   [-l 1|2] [-b BYTES] [-j N] [--no-check] IN OUT\n"
        "  amdcf decompress [-j N] [--max-output BYTES] IN OUT\n"
        "  amdcf rawc [-l 1|2] IN OUT           raw LZ4 block (interop/testing)\n"
        "  amdcf rawd -s SIZE IN OUT            decode a raw LZ4 block of known decoded size\n"
        "  amdcf bench      [-l 1|2] [-j N] [-r REPS] FILE      in-memory compress/decompress throughput\n"
        "  amdcf ramsim     [-l 1|2] [-p PAGE] [-r REPS] FILE... split files into pages, compress each page\n"
        "                   like a RAM compressor would, report RAM saved and per-page decode latency\n"
        "'-' = stdin/stdout. Level 1 = fastest compress, level 2 = smaller (same decode speed).\n",
        stderr);
    return 64;
}

static int cmd_bench(const Bytes& in, int level, unsigned jobs, int reps) {
    const double mb = double(in.size()) / 1e6;
    Bytes c;
    auto t = Clock::now();
    for (int i = 0; i < reps; ++i) c = compress(in, kDefaultBlock, level, jobs, true);
    double ct = since(t) / reps;
    Bytes d(in.size());
    t = Clock::now();
    for (int i = 0; i < reps; ++i) decompress_into(c.data(), c.size(), d.data(), d.size(), jobs);
    double dt = since(t) / reps;
    // single-thread raw block decode (no container, no checksum): the pure decoder speed
    Bytes bc(block_bound(in.size())), bd(in.size());
    size_t bn = block_compress(in.data(), in.size(), bc.data(), level);
    t = Clock::now();
    for (int i = 0; i < reps; ++i)
        if (!block_decompress(bc.data(), bn, bd.data(), in.size())) throw std::runtime_error("block decode failed");
    double bt = since(t) / reps;
    std::printf("input %zu B -> %zu B (%.2f%%); container round-trip %s\n", in.size(), c.size(),
                100.0 * double(c.size()) / double(std::max<size_t>(1, in.size())), d == in ? "exact" : "MISMATCH");
    std::printf("compress   (container, -j %u): %8.0f MB/s\n", jobs, mb / ct);
    std::printf("decompress (container, -j %u, into preallocated buffer, checksum verified): %8.0f MB/s\n", jobs, mb / dt);
    std::printf("decompress (single block, 1 thread, no checksum): %8.0f MB/s  (%zu B -> %zu B)\n", mb / bt, bn, in.size());
    return d == in && bd == in ? 0 : 2;
}

static int cmd_ramsim(const std::vector<std::string>& files, int level, size_t page, int reps) {
    Bytes mem;
    for (const auto& f : files) { Bytes b = read_all(f); mem.insert(mem.end(), b.begin(), b.end()); }
    mem.resize(mem.size() / page * page);
    const size_t np = mem.size() / page;
    if (!np) { std::fputs("need at least one full page of input\n", stderr); return 64; }
    CompressedPageCache cache(page, level);
    size_t zero = 0, fill = 0, lz = 0, raw = 0;
    auto t = Clock::now();
    for (size_t i = 0; i < np; ++i) cache.put(i, mem.data() + i * page);
    double ct = since(t);
    // classify (re-encode once to count kinds)
    Bytes tmp(page_encode_bound(page));
    for (size_t i = 0; i < np; ++i) {
        size_t n = page_compress(mem.data() + i * page, page, tmp.data(), level);
        if (!n) ++raw; else if (tmp[0] == PAGE_ZERO) ++zero; else if (tmp[0] == PAGE_FILL) ++fill; else ++lz;
    }
    // random-order page faults
    std::vector<size_t> order(np);
    for (size_t i = 0; i < np; ++i) order[i] = i;
    std::mt19937_64 rng(1);
    std::shuffle(order.begin(), order.end(), rng);
    Bytes out(page);
    t = Clock::now();
    for (int r = 0; r < reps; ++r)
        for (size_t i : order) {
            cache.get(i, out.data());
            if (std::memcmp(out.data(), mem.data() + i * page, page) != 0) { std::fputs("MISMATCH\n", stderr); return 2; }
        }
    double dt = since(t) / reps;
    const double raw_mb = double(mem.size()) / 1048576.0, comp_mb = double(cache.compressed_bytes()) / 1048576.0;
    std::printf("pages: %zu x %zu B = %.1f MiB   (zero %zu, fill %zu, compressed %zu, kept raw %zu)\n", np, page, raw_mb, zero, fill, lz, raw);
    std::printf("RAM used compressed: %.1f MiB  ->  saves %.1f MiB (%.1f%%)\n", comp_mb, raw_mb - comp_mb, 100.0 * (1.0 - comp_mb / raw_mb));
    std::printf("page-out (compress):   %8.0f MB/s   %.2f us/page\n", raw_mb * 1.048576 / ct, ct * 1e6 / double(np));
    std::printf("page-in  (decompress, random order, incl. lookup+verify): %8.0f MB/s   %.2f us/page\n",
                raw_mb * 1.048576 / dt, dt * 1e6 / double(np));
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);    // '-' must be byte-exact: no CRLF translation
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    int level = 1, reps = 5;
    size_t bs = kDefaultBlock, page = 4096, rawsize = 0;
    unsigned jobs = 0;
    uint64_t maxout = uint64_t(1) << 32;
    bool check = true;
    std::vector<std::string> pos;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> unsigned long long {
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a);
            return std::strtoull(argv[++i], nullptr, 10);
        };
        try {
            if (a == "-l") level = int(val());
            else if (a == "-b") bs = size_t(val());
            else if (a == "-j") jobs = unsigned(val());
            else if (a == "-r") reps = std::max(1, int(val()));
            else if (a == "-p") page = size_t(val());
            else if (a == "-s") rawsize = size_t(val());
            else if (a == "--max-output") maxout = val();
            else if (a == "--no-check") check = false;
            else if (a == "-" || a[0] != '-') pos.push_back(a);
            else return usage();
        } catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 64; }
    }
    if (level < 1 || level > 2 || page < 64) return usage();
    try {
        if (cmd == "compress" && pos.size() == 2) {
            auto t = Clock::now();
            Bytes in = read_all(pos[0]);
            Bytes out = compress(in, bs, level, jobs, check);
            write_all(pos[1], out);
            std::fprintf(stderr, "%zu -> %zu bytes (%.2f%%) in %.2fs\n", in.size(), out.size(),
                         in.empty() ? 0.0 : 100.0 * double(out.size()) / double(in.size()), since(t));
        } else if (cmd == "decompress" && pos.size() == 2) {
            write_all(pos[1], decompress(read_all(pos[0]), maxout, jobs));
        } else if (cmd == "rawc" && pos.size() == 2) {
            Bytes in = read_all(pos[0]), out(block_bound(in.size()));
            out.resize(block_compress(in.data(), in.size(), out.data(), level));
            write_all(pos[1], out);
        } else if (cmd == "rawd" && pos.size() == 2) {
            Bytes in = read_all(pos[0]), out(rawsize);
            if (!block_decompress(in.data(), in.size(), out.data(), rawsize)) throw std::runtime_error("corrupt block");
            write_all(pos[1], out);
        } else if (cmd == "bench" && pos.size() == 1) {
            return cmd_bench(read_all(pos[0]), level, jobs ? jobs : 1, reps);
        } else if (cmd == "ramsim" && !pos.empty()) {
            return cmd_ramsim(pos, level, page, reps);
        } else {
            return usage();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
