// amdcb: BWT-tier compressor CLI.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "bwtcodec.hpp"

using namespace amdc1::bwtc;
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

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::invalid_argument("usage");
        const std::string cmd = argv[1];
        size_t bs = kDefaultBlock;
        unsigned jobs = 0;
        bool verify = false;
        uint64_t maxout = uint64_t(1) << 32;
        std::vector<std::string> pos;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            auto val = [&]() -> unsigned long long { if (i + 1 >= argc) throw std::invalid_argument("missing value"); return std::strtoull(argv[++i], nullptr, 10); };
            if (a == "-b") bs = size_t(val());
            else if (a == "-j") jobs = unsigned(val());
            else if (a == "--max-output") maxout = val();
            else if (a == "--verify") verify = true;
            else pos.push_back(a);
        }
        if (cmd == "compress" && pos.size() == 2) {
            Bytes in = read_all(pos[0]);
            auto t = Clock::now();
            Bytes out = compress(in, bs, jobs);
            const double dt = since(t);
            if (verify && decompress(out, maxout, jobs) != in) throw std::runtime_error("verify failed");
            write_all(pos[1], out);
            std::fprintf(stderr, "%zu -> %zu bytes (%.2f%%) in %.2fs (%.1f MB/s)\n", in.size(), out.size(),
                         in.empty() ? 0.0 : 100.0 * double(out.size()) / double(in.size()), dt, double(in.size()) / dt / 1e6);
        } else if (cmd == "decompress" && pos.size() == 2) {
            Bytes in = read_all(pos[0]);
            auto t = Clock::now();
            Bytes out = decompress(in, maxout, jobs);
            const double dt = since(t);
            write_all(pos[1], out);
            std::fprintf(stderr, "%zu bytes in %.2fs (%.1f MB/s)\n", out.size(), dt, double(out.size()) / dt / 1e6);
        } else {
            throw std::invalid_argument("usage");
        }
    } catch (const std::invalid_argument&) {
        std::fprintf(stderr, "usage:\n  amdcb compress   [-b BYTES] [-j N] [--verify] IN OUT\n  amdcb decompress [--max-output BYTES] [-j N] IN OUT\n'-' means stdin/stdout.\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
