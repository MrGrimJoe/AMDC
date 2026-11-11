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
#include <string>

#include "amdc1.hpp"

using amdc1::Bytes;

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
    if (path == "-") {
        std::fwrite(d.data(), 1, d.size(), stdout);
        return;
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}

static int usage() {
    std::fputs(
        "amdc1 - AMDC-1 lossless compressor\n"
        "  amdc1 compress   [-b BYTES] [-l 0|1|2] [-m BITS] [-j N] [--verify] IN OUT\n"
        "  amdc1 decompress [--max-output BYTES] [-j N] IN OUT\n"
        "'-' means stdin/stdout. Levels: 0 lite model (fast), 1 full model + sampled search (default), 2 exhaustive.\n"
        "-m BITS: log2 slots per model table (16..24, default 22). Memory ~ 10 x 4 x 2^BITS bytes (full), half for -l 0.\n",
        stderr);
    return 64;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);    // '-' must be byte-exact: no CRLF translation
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 4) return usage();
    std::string cmd = argv[1];
    size_t bs = amdc1::kDefaultBlock;
    int level = 1, membits = amdc1::kDefaultMemBits;
    unsigned jobs = 0;
    uint64_t maxout = uint64_t(1) << 32;
    bool verify = false;
    std::vector<std::string> pos;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> unsigned long long {
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a);
            return std::strtoull(argv[++i], nullptr, 10);
        };
        try {
            if (a == "-b") bs = size_t(val());
            else if (a == "-l") level = int(val());
            else if (a == "-m") membits = int(val());
            else if (a == "-j") jobs = unsigned(val());
            else if (a == "--max-output") maxout = val();
            else if (a == "--verify") verify = true;
            else if (a == "-" || a[0] != '-') pos.push_back(a);
            else return usage();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 64;
        }
    }
    if (pos.size() != 2 || level < 0 || level > 2) return usage();
    try {
        auto t0 = std::chrono::steady_clock::now();
        Bytes in = read_all(pos[0]);
        if (cmd == "compress") {
            Bytes out = amdc1::compress(in, bs, level, jobs, membits);
            if (verify && amdc1::decompress(out, uint64_t(-1), jobs) != in) {
                std::fputs("VERIFY FAILED\n", stderr);
                return 2;
            }
            write_all(pos[1], out);
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "%zu -> %zu bytes (%.2f%%) in %.2fs\n", in.size(), out.size(),
                         in.empty() ? 0.0 : 100.0 * double(out.size()) / double(in.size()), sec);
        } else if (cmd == "decompress") {
            write_all(pos[1], amdc1::decompress(in, maxout, jobs));
        } else {
            return usage();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
