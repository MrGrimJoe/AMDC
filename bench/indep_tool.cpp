// Baseline for the solid-mode benchmark: the EXISTING AMDC-F independent-file compression, one AMDF container per file,
// exactly the library call `amdcf compress` makes (256 KiB blocks, checksum on, default threads).
//   indep pack [-l 1|2] DIR OUTDIR      writes OUTDIR/<relative path>.amdf for every regular file, prints total bytes
//   indep unpack INDIR DESTDIR          decodes every .amdf back to DESTDIR/<relative path>
//   indep bench [-l 1|2] DIR            in-memory throughput, no disk I/O: raw, compressed bytes, MB/s compress, MB/s decompress
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../src/fast.hpp"

namespace fs = std::filesystem;
using namespace amdc1::fast;

static Bytes slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
static void spit(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

int main(int argc, char** argv) {
    int level = 1, a = 1;
    if (argc < 4) return 64;
    const std::string cmd = argv[a++];
    if ((cmd == "pack" || cmd == "bench") && !std::strcmp(argv[a], "-l")) { level = std::atoi(argv[a + 1]); a += 2; }
    if (cmd == "bench") {  // in-memory, no disk I/O: independent per-file compress then decompress, best of 3
        std::vector<Bytes> raws, comps;
        unsigned long long raw = 0, csz = 0;
        for (auto& e : fs::recursive_directory_iterator(fs::canonical(argv[a])))
            if (!e.is_symlink() && e.is_regular_file()) { raws.push_back(slurp(e.path())); raw += raws.back().size(); }
        double bc = 1e9, bd = 1e9;
        for (int r = 0; r < 3; ++r) {
            auto t = std::chrono::steady_clock::now(); comps.clear(); csz = 0;
            for (auto& in : raws) { comps.push_back(compress(in, kDefaultBlock, level, 0, true)); csz += comps.back().size(); }
            bc = std::min(bc, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
            t = std::chrono::steady_clock::now();
            for (size_t i = 0; i < comps.size(); ++i) if (decompress(comps[i]) != raws[i]) return 2;
            bd = std::min(bd, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
        }
        std::printf("%llu\t%llu\t%.0f\t%.0f\n", raw, csz, double(raw) / 1e6 / bc, double(raw) / 1e6 / bd);
        return 0;
    }
    const fs::path src = fs::canonical(argv[a]), dst = argv[a + 1];
    unsigned long long total = 0, raw = 0, files = 0;
    for (auto& e : fs::recursive_directory_iterator(src)) {
        if (e.is_symlink() || !e.is_regular_file()) continue;
        const fs::path rel = fs::relative(e.path(), src);
        if (cmd == "pack") {
            const Bytes in = slurp(e.path());
            const Bytes c = compress(in, kDefaultBlock, level, 0, true);
            spit(dst / (rel.string() + ".amdf"), c);
            total += c.size(); raw += in.size(); ++files;
        } else {
            fs::path r = rel; r.replace_extension("");
            spit(dst / r, decompress(slurp(e.path())));
            ++files;
        }
    }
    if (cmd == "pack") std::printf("%llu files, %llu B -> %llu B\n", files, raw, total);
    return 0;
}
