// amdcs: AMDC-S solid multi-file archiver (optional; ordinary AMDC-F is untouched).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "solid.hpp"

using namespace amdc1::solid;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

static Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static int usage() {
    std::fputs(
        "amdcs - AMDC-S solid multi-file archive (LZ4-format stripes, cross-file matches)\n"
        "  amdcs pack   [-g FILES] [-u PAGES] [-s STRIPE_KiB] [-l 1|2] [-j N] [-o name|ext|size|extsize] DIR OUT\n"
        "  amdcs unpack [-j N] [-f] [--max-output BYTES] IN DIR     (refuses to overwrite unless -f)\n"
        "  amdcs list   IN\n"
        "  amdcs bench  [pack options] DIR     in-memory pack/unpack throughput (no disk I/O), verified\n"
        "  -g files per group (default 4; 1 = no cross-file sharing)   -u interleave unit in 4 KiB pages (default 1; 0 = whole files)\n"
        "  -s stripe size in KiB (default 256)   -o file order before grouping (default name)\n",
        stderr);
    return 64;
}

static std::string ext_of(const std::string& p) {
    const size_t s = p.rfind('/'), d = p.rfind('.');
    return (d == std::string::npos || (s != std::string::npos && d < s)) ? std::string() : p.substr(d);
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    try {
        const std::string cmd = argv[1];
        Options opt;
        std::string order = "name";
        bool force = false;
        uint64_t max_output = uint64_t(1) << 32;
        std::vector<std::string> pos;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> const char* { if (i + 1 >= argc) throw std::runtime_error("missing value for " + a); return argv[++i]; };
            if (a == "-g") opt.group = unsigned(std::atoi(next()));
            else if (a == "-u") opt.unit_pages = unsigned(std::atoi(next()));
            else if (a == "-s") opt.stripe_bytes = size_t(std::atoll(next())) << 10;
            else if (a == "-l") opt.level = std::atoi(next());
            else if (a == "-j") opt.threads = unsigned(std::atoi(next()));
            else if (a == "-o") order = next();
            else if (a == "-f") force = true;
            else if (a == "--max-output") max_output = std::strtoull(next(), nullptr, 10);
            else if (!a.empty() && a[0] == '-') return usage();
            else pos.push_back(a);
        }
        auto collect = [&](const std::string& dir, size_t& skipped) {
            std::vector<File> files;
            const fs::path root = fs::canonical(dir);
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied); it != fs::recursive_directory_iterator(); ++it) {
                if (it->is_symlink() || !it->is_regular_file()) { if (!it->is_directory()) ++skipped; continue; }
                File f;
                f.path = fs::relative(it->path(), root).generic_string();
                f.data = read_file(it->path());
                files.push_back(std::move(f));
            }
            std::sort(files.begin(), files.end(), [&](const File& a, const File& b) {
                if (order == "ext") { const auto ea = ext_of(a.path), eb = ext_of(b.path); if (ea != eb) return ea < eb; }
                else if (order == "size") { if (a.data.size() != b.data.size()) return a.data.size() < b.data.size(); }
                else if (order == "extsize") {
                    const auto ea = ext_of(a.path), eb = ext_of(b.path);
                    if (ea != eb) return ea < eb;
                    if (a.data.size() != b.data.size()) return a.data.size() < b.data.size();
                } else if (order != "name") throw std::runtime_error("unknown order " + order);
                return a.path < b.path;
            });
            return files;
        };
        if (cmd == "bench" && pos.size() == 1) {
            size_t skipped = 0;
            const std::vector<File> files = collect(pos[0], skipped);
            size_t raw = 0;
            for (const File& f : files) raw += f.data.size();
            double bp = 1e9, bu = 1e9;
            Bytes a;
            for (int r = 0; r < 3; ++r) {
                auto t = Clock::now(); a = pack(files, opt); bp = std::min(bp, since(t));
                t = Clock::now(); const auto back = unpack(a.data(), a.size(), max_output, opt.threads); bu = std::min(bu, since(t));
                if (back.size() != files.size()) return 2;
                for (size_t i = 0; i < files.size(); ++i) if (back[i].path != files[i].path || back[i].data != files[i].data) { std::fputs("MISMATCH\n", stderr); return 2; }
            }
            std::printf("%zu\t%zu\t%.0f\t%.0f\n", raw, a.size(), double(raw) / 1e6 / bp, double(raw) / 1e6 / bu);
            return 0;
        }
        if (cmd == "pack" && pos.size() == 2) {
            const auto t0 = Clock::now();
            size_t skipped = 0;
            std::vector<File> files = collect(pos[0], skipped);
            size_t raw = 0;
            for (const File& f : files) raw += f.data.size();
            const Bytes a = pack(files, opt);
            std::ofstream o(pos[1], std::ios::binary);
            if (!o) throw std::runtime_error("cannot write " + pos[1]);
            o.write(reinterpret_cast<const char*>(a.data()), std::streamsize(a.size()));
            std::fprintf(stderr, "%zu files (%zu skipped non-regular), %zu B -> %zu B (%.2f%%) in %.2fs\n", files.size(), skipped, raw, a.size(),
                         raw ? 100.0 * double(a.size()) / double(raw) : 0.0, since(t0));
            return 0;
        }
        if (cmd == "unpack" && pos.size() == 2) {
            const auto t0 = Clock::now();
            const Bytes a = read_file(pos[0]);
            const auto files = unpack(a.data(), a.size(), max_output, opt.threads);
            extract_to(pos[1], files, force);
            std::fprintf(stderr, "extracted %zu files in %.2fs\n", files.size(), since(t0));
            return 0;
        }
        if (cmd == "list" && pos.size() == 1) {
            const Bytes a = read_file(pos[0]);
            for (const File& f : unpack(a.data(), a.size(), max_output, opt.threads)) std::printf("%10zu  %s\n", f.data.size(), f.path.c_str());
            return 0;
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "amdcs: %s\n", e.what());
        return 1;
    }
}
