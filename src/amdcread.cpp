// amdcread: tiny reader for every AMDC-family file. Recognises the format by its magic bytes, not the file name:
//   AMDC  context-mixing archive (amdc1)      AMDF  fast archive (amdcf)
//   AMDB  BWT archive (amdcb)                 AMDS  solid multi-file archive (amdcs)
// Commands:
//   amdcread info FILE...            what each file is (no decoding)
//   amdcread test FILE...            decode fully, verify, report sizes and ratio
//   amdcread cat FILE [NAME]         decoded bytes to stdout (AMDS needs NAME, see ls)
//   amdcread ls FILE                 list the files in an AMDS archive
//   amdcread view FILE [NAME]        decode and show the first 512 bytes as text or hex
//   amdcread scan [DIR...]           find AMDC-family files below DIR (default .) by magic, any extension
// Options: -m MB   refuse to decode more than MB megabytes of output (default 1024)
// Read-only: never writes anything but stdout, never follows symlinks while scanning.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "amdc1.hpp"
#include "bwtcodec.hpp"
#include "fast.hpp"
#include "solid.hpp"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

enum class Fmt { None, Cm, Fast, Bwt, Solid };
static const char* fmt_name(Fmt f) {
    switch (f) {
        case Fmt::Cm: return "AMDC (context-mixing, amdc1)";
        case Fmt::Fast: return "AMDF (fast, amdcf)";
        case Fmt::Bwt: return "AMDB (BWT, amdcb)";
        case Fmt::Solid: return "AMDS (solid multi-file, amdcs)";
        default: return "not an AMDC-family file";
    }
}
static Fmt detect(const uint8_t* b, size_t n) {
    if (n < 4) return Fmt::None;
    if (!std::memcmp(b, "AMDC", 4)) return Fmt::Cm;
    if (!std::memcmp(b, "AMDF", 4)) return Fmt::Fast;
    if (!std::memcmp(b, "AMDB", 4)) return Fmt::Bwt;
    if (!std::memcmp(b, "AMDS", 4)) return Fmt::Solid;
    return Fmt::None;
}
static bool read_file(const std::string& p, Bytes& out, size_t limit = size_t(1) << 31) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto sz = in.tellg();
    if (sz < 0 || uint64_t(sz) > limit) return false;
    in.seekg(0);
    out.resize(size_t(sz));
    in.read(reinterpret_cast<char*>(out.data()), sz);
    return bool(in) || out.empty();
}
static Fmt detect_file(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    uint8_t m[4] = {0};
    if (!in.read(reinterpret_cast<char*>(m), 4)) return Fmt::None;
    return detect(m, 4);
}

static uint64_t g_max = uint64_t(1024) << 20;

// Decodes a single-stream archive. Throws on malformed input.
static Bytes decode_stream(Fmt f, const Bytes& in) {
    switch (f) {
        case Fmt::Cm: return amdc1::decompress(in, g_max);
        case Fmt::Fast: return amdc1::fast::decompress(in, g_max);
        case Fmt::Bwt: return amdc1::bwtc::decompress(in, g_max);
        default: throw std::runtime_error("not a single-stream archive");
    }
}

static int cmd_info(const std::vector<std::string>& files) {
    int bad = 0;
    for (const auto& p : files) {
        Bytes b;
        std::error_code ec;
        const auto sz = fs::file_size(p, ec);
        const Fmt f = detect_file(p);
        std::printf("%s: %s, %llu bytes", p.c_str(), fmt_name(f), (unsigned long long)(ec ? 0 : sz));
        if (f == Fmt::Fast && read_file(p, b, size_t(1) << 28)) {
            try { std::printf(", decodes to %zu bytes", amdc1::fast::decompressed_size(b.data(), b.size())); } catch (...) { std::printf(", header damaged"); bad = 1; }
        }
        std::printf("\n");
        if (f == Fmt::None) bad = 1;
    }
    return bad;
}

static int cmd_test(const std::vector<std::string>& files) {
    int bad = 0;
    for (const auto& p : files) {
        Bytes in;
        if (!read_file(p, in)) { std::printf("%s: cannot read\n", p.c_str()); bad = 1; continue; }
        const Fmt f = detect(in.data(), in.size());
        try {
            if (f == Fmt::Solid) {
                const auto fl = amdc1::solid::unpack(in.data(), in.size(), g_max);
                size_t tot = 0;
                for (auto& x : fl) tot += x.data.size();
                std::printf("%s: OK %s, %zu files, %zu bytes -> ratio %.2f%%\n", p.c_str(), fmt_name(f), fl.size(), tot, tot ? 100.0 * double(in.size()) / double(tot) : 0.0);
            } else if (f != Fmt::None) {
                const Bytes o = decode_stream(f, in);
                std::printf("%s: OK %s, %zu bytes -> %zu bytes (%.2f%%)\n", p.c_str(), fmt_name(f), in.size(), o.size(), o.empty() ? 0.0 : 100.0 * double(in.size()) / double(o.size()));
            } else { std::printf("%s: %s\n", p.c_str(), fmt_name(f)); bad = 1; }
        } catch (const std::exception& e) { std::printf("%s: DAMAGED or refused (%s)\n", p.c_str(), e.what()); bad = 1; }
    }
    return bad;
}

static bool load_decoded(const std::string& p, const char* name, Bytes& out, std::string& label) {
    Bytes in;
    if (!read_file(p, in)) { std::fprintf(stderr, "amdcread: cannot read %s\n", p.c_str()); return false; }
    const Fmt f = detect(in.data(), in.size());
    try {
        if (f == Fmt::Solid) {
            const auto fl = amdc1::solid::unpack(in.data(), in.size(), g_max);
            if (!name) { std::fprintf(stderr, "amdcread: %s is a multi-file archive; give a file name (see: amdcread ls)\n", p.c_str()); return false; }
            for (auto& x : fl) if (x.path == name) { out = x.data; label = x.path; return true; }
            std::fprintf(stderr, "amdcread: no file '%s' in %s\n", name, p.c_str());
            return false;
        }
        if (f == Fmt::None) { std::fprintf(stderr, "amdcread: %s is not an AMDC-family file\n", p.c_str()); return false; }
        out = decode_stream(f, in);
        label = p;
        return true;
    } catch (const std::exception& e) { std::fprintf(stderr, "amdcread: %s: %s\n", p.c_str(), e.what()); return false; }
}

static int cmd_cat(const std::string& p, const char* name) {
    Bytes o; std::string label;
    if (!load_decoded(p, name, o, label)) return 1;
    return std::fwrite(o.data(), 1, o.size(), stdout) == o.size() ? 0 : 1;
}

static int cmd_ls(const std::string& p) {
    Bytes in;
    if (!read_file(p, in) || detect(in.data(), in.size()) != Fmt::Solid) { std::fprintf(stderr, "amdcread: %s is not an AMDS archive\n", p.c_str()); return 1; }
    try {
        for (auto& x : amdc1::solid::unpack(in.data(), in.size(), g_max)) std::printf("%10zu  %s\n", x.data.size(), x.path.c_str());
    } catch (const std::exception& e) { std::fprintf(stderr, "amdcread: %s: %s\n", p.c_str(), e.what()); return 1; }
    return 0;
}

static int cmd_view(const std::string& p, const char* name) {
    Bytes o; std::string label;
    if (!load_decoded(p, name, o, label)) return 1;
    const size_t n = std::min<size_t>(o.size(), 512);
    bool text = true;
    for (size_t i = 0; i < n; ++i) if ((o[i] < 32 && o[i] != '\n' && o[i] != '\r' && o[i] != '\t') || o[i] > 126) { text = false; break; }
    std::printf("%s: %zu bytes decoded; first %zu as %s\n", label.c_str(), o.size(), n, text ? "text" : "hex");
    if (text) { std::fwrite(o.data(), 1, n, stdout); std::printf("\n"); }
    else for (size_t i = 0; i < n; i += 16) {
        std::printf("%08zx  ", i);
        for (size_t j = i; j < i + 16; ++j) j < n ? std::printf("%02x ", o[j]) : std::printf("   ");
        std::printf(" ");
        for (size_t j = i; j < std::min(i + 16, n); ++j) std::printf("%c", o[j] >= 32 && o[j] < 127 ? o[j] : '.');
        std::printf("\n");
    }
    return 0;
}

static int cmd_scan(std::vector<std::string> dirs) {
    if (dirs.empty()) dirs.push_back(".");
    size_t found = 0;
    for (const auto& d : dirs) {
        std::error_code ec;
        fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2) || it->is_symlink(e2)) continue;
            const auto sz = it->file_size(e2);
            if (e2 || sz < 8) continue;
            const Fmt f = detect_file(it->path().string());
            if (f == Fmt::None) continue;
            std::printf("%-34s %12llu  %s\n", fmt_name(f), (unsigned long long)sz, it->path().string().c_str());
            ++found;
        }
    }
    std::fprintf(stderr, "%zu AMDC-family file(s) found\n", found);
    return 0;
}

static int usage() {
    std::fprintf(stderr,
        "amdcread - reader for AMDC, AMDF, AMDB and AMDS files (detected by content, any file name)\n"
        "  amdcread info FILE...     what each file is\n"
        "  amdcread test FILE...     decode and verify, show sizes\n"
        "  amdcread view FILE [NAME] show the first 512 decoded bytes (text or hex)\n"
        "  amdcread cat FILE [NAME]  write the decoded bytes to stdout\n"
        "  amdcread ls FILE          list files in an AMDS archive\n"
        "  amdcread scan [DIR...]    find AMDC-family files below DIR (default .)\n"
        "  -m MB                     refuse to decode more than MB megabytes (default 1024)\n"
        "A single FILE argument with no command means: view FILE.\n");
    return 64;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);   // `cat` writes raw bytes: no LF -> CRLF translation
#endif
    std::vector<std::string> a;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-m") && i + 1 < argc) g_max = uint64_t(std::strtoull(argv[++i], nullptr, 10)) << 20;
        else a.push_back(argv[i]);
    }
    if (a.empty()) return usage();
    const std::string c = a[0];
    std::vector<std::string> rest(a.begin() + 1, a.end());
    if (c == "info" && !rest.empty()) return cmd_info(rest);
    if (c == "test" && !rest.empty()) return cmd_test(rest);
    if (c == "scan") return cmd_scan(rest);
    if (c == "ls" && rest.size() == 1) return cmd_ls(rest[0]);
    if (c == "cat" && (rest.size() == 1 || rest.size() == 2)) return cmd_cat(rest[0], rest.size() > 1 ? rest[1].c_str() : nullptr);
    if (c == "view" && (rest.size() == 1 || rest.size() == 2)) return cmd_view(rest[0], rest.size() > 1 ? rest[1].c_str() : nullptr);
    if (a.size() == 1 && fs::exists(a[0])) return cmd_view(a[0], nullptr);
    return usage();
}
