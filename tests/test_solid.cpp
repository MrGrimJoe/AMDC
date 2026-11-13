// AMDC-S tests. Exit status 0 = all passed. Also run under ASan/UBSan (make sanitize).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "../src/solid.hpp"

using namespace amdc1::solid;
namespace fs = std::filesystem;
using amdc1::fast::hash64;

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define CHECK_THROWS(expr) do { ++g_checks; bool t_ = false; try { expr; } catch (const SolidError&) { t_ = true; } if (!t_) { ++g_fail; std::printf("FAIL line %d: did not throw: %s\n", __LINE__, #expr); } } while (0)

static std::mt19937 rng(12345);

static Bytes texty(size_t n) {
    static const char* w[] = {"#include ", "int ", "return ", "static ", "const ", "struct ", "if (x) {\n", "}\n", "foo_bar", "uint8_t "};
    std::string s;
    while (s.size() < n) s += w[rng() % 10];
    return Bytes(s.begin(), s.begin() + n);
}
static Bytes randb(size_t n) { Bytes b(n); for (auto& c : b) c = uint8_t(rng()); return b; }

static std::vector<File> make_files() {
    std::vector<File> f;
    const size_t sizes[] = {0, 1, 100, 4095, 4096, 4097, 8192, 12345, 3000, 70000, 0, 4096, 150000, 5000, 4097, 9000, 66000, 1, 20000};
    int i = 0;
    Bytes shared = texty(30000);
    for (size_t sz : sizes) {
        File x;
        x.path = (i % 3 == 0 ? "dir/sub/" : i % 3 == 1 ? "dir/" : "") + std::string("file") + std::to_string(i) + (i % 2 ? ".c" : ".bin");
        switch (i % 4) {
            case 0: x.data = texty(sz); break;
            case 1: x.data = randb(sz); break;
            case 2: x.data = Bytes(sz, uint8_t(i)); break;
            default: x.data.assign(shared.begin(), shared.begin() + std::min(sz, shared.size())); x.data.resize(sz, 7); break;
        }
        f.push_back(std::move(x));
        ++i;
    }
    return f;
}

static bool same(const std::vector<File>& a, const std::vector<File>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].path != b[i].path || a[i].data != b[i].data) return false;
    return true;
}
static void fix_trailer(Bytes& a) {
    if (a.size() < 8) return;
    const uint64_t h = hash64(a.data(), a.size() - 8);
    for (int i = 0; i < 8; ++i) a[a.size() - 8 + i] = uint8_t(h >> (8 * i));
}

static void test_roundtrip_matrix() {
    const auto files = make_files();
    for (unsigned G : {1u, 2u, 3u, 4u, 8u, 16u})
        for (unsigned U : {0u, 1u, 2u, 5u})
            for (size_t SB : {size_t(4096), size_t(8192), size_t(65536), size_t(262144)}) {
                Options o; o.group = G; o.unit_pages = U; o.stripe_bytes = SB; o.level = (G % 2) ? 1 : 2;
                o.threads = 1;
                const Bytes a1 = pack(files, o);
                o.threads = 3;
                const Bytes a3 = pack(files, o);
                CHECK(a1 == a3);                                   // identical for any thread count
                CHECK(same(unpack(a1.data(), a1.size(), uint64_t(1) << 30, 1), files));
                CHECK(same(unpack(a1.data(), a1.size(), uint64_t(1) << 30, 3), files));
            }
}

static void test_edge_sets() {
    Options o;
    std::vector<File> none;
    Bytes a = pack(none, o);
    CHECK(unpack(a.data(), a.size()).empty());
    std::vector<File> one{{"only_empty", {}}};
    a = pack(one, o);
    CHECK(same(unpack(a.data(), a.size()), one));
    std::vector<File> allzero{{"z1", Bytes(1 << 20, 0)}, {"z2", Bytes(300000, 0)}, {"e", {}}};
    a = pack(allzero, o);
    CHECK(a.size() < 500);                                          // fill stripes
    CHECK(same(unpack(a.data(), a.size()), allzero));
    std::vector<File> inc{{"r1", randb(100000)}, {"r2", randb(100000)}};
    a = pack(inc, o);
    CHECK(same(unpack(a.data(), a.size()), inc));
    CHECK(a.size() < 200000 + 400);                                 // raw stripes: bounded overhead
}

static void test_path_rules() {
    Options o;
    CHECK_THROWS(pack({{"a", {}}, {"a", {}}}, o));                  // duplicate
    CHECK_THROWS(pack({{"a", {}}, {"a/b", {}}}, o));                // file vs directory
    for (const char* bad : {"", "/abs", "../x", "a/../b", "a//b", "./a", "a/.", "a\\b", "C:x", "a/", "x\ny", "a/..", ".."})
        CHECK_THROWS(pack({{bad, {}}}, o));
    CHECK(path_is_safe("a/b/c.txt") && path_is_safe("..a") && path_is_safe("a..b") && path_is_safe(".hidden"));
    CHECK(!path_is_safe(std::string(256, 'a')));
    // hostile archives built with validation off must be rejected by unpack()
    for (const char* bad : {"../evil", "/abs", "a/../../b", "a\\b", "C:x", "a//b"}) {
        std::vector<File> f{{bad, Bytes{1, 2, 3}}};
        const Bytes a = pack(f, o, false);
        CHECK_THROWS(unpack(a.data(), a.size()));
    }
    for (auto fs_ : {std::vector<File>{{"d", {1}}, {"d", {2}}}, std::vector<File>{{"a", {1}}, {"a/b", {2}}}}) {
        const Bytes a = pack(fs_, o, false);
        CHECK_THROWS(unpack(a.data(), a.size()));
    }
}

static void test_limits() {
    const auto files = make_files();
    Options o;
    const Bytes a = pack(files, o);
    CHECK_THROWS(unpack(a.data(), a.size(), 1000));                  // max_output
    CHECK_THROWS(unpack(a.data(), 5));
    Bytes b = a; b[0] = 'X'; CHECK_THROWS(unpack(b.data(), b.size()));
    b = a; b[4] = 2; fix_trailer(b); CHECK_THROWS(unpack(b.data(), b.size()));       // version
    b = a; b[5] = 1; fix_trailer(b); CHECK_THROWS(unpack(b.data(), b.size()));       // flags
    b = a; b.push_back(0); CHECK_THROWS(unpack(b.data(), b.size()));                 // trailing byte
    for (size_t cut : {size_t(1), size_t(8), size_t(9), a.size() / 2}) {
        Bytes t(a.begin(), a.end() - cut);
        CHECK_THROWS(unpack(t.data(), t.size()));
    }
}

// Random corruption: every outcome must be "rejected" or "identical to the original" - never a crash or silent change.
static void test_mutation_fuzz() {
    const auto files = make_files();
    int accepted = 0, rejected = 0;
    for (unsigned G : {1u, 4u, 16u})
        for (unsigned U : {0u, 1u}) {
            Options o; o.group = G; o.unit_pages = U; o.stripe_bytes = 8192;
            const Bytes base = pack(files, o);
            for (int it = 0; it < 1500; ++it) {
                Bytes m = base;
                switch (rng() % 4) {
                    case 0: m[rng() % m.size()] ^= uint8_t(1u << (rng() % 8)); break;
                    case 1: for (int k = 1 + rng() % 3; k--;) m[rng() % m.size()] = uint8_t(rng()); break;
                    case 2: m.resize(rng() % m.size()); break;
                    default: m.insert(m.begin() + rng() % m.size(), uint8_t(rng())); break;
                }
                if (rng() % 2) fix_trailer(m);                       // get past the archive checksum, hit the parsers
                try {
                    const auto got = unpack(m.data(), m.size(), uint64_t(64) << 20, 2);
                    ++accepted;
                    CHECK(same(got, files));
                } catch (const SolidError&) { ++rejected; }
            }
        }
    CHECK(rejected > 0);
    std::printf("mutation fuzz: %d rejected, %d accepted (identical)\n", rejected, accepted);
}

static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }

static void test_extraction_safety() {
    const fs::path tmp = fs::temp_directory_path() / ("amdcs_test_" + std::to_string(rng()));
    fs::create_directories(tmp / "out");
    fs::create_directories(tmp / "outside");
    { std::ofstream(tmp / "outside" / "secret") << "SECRET"; }
    const std::string dest = (tmp / "out").string();
    const auto files = make_files();
    extract_to(dest, files);
    for (const File& f : files) { CHECK(fs::exists(tmp / "out" / f.path)); CHECK(slurp(tmp / "out" / f.path) == std::string(f.data.begin(), f.data.end())); }
    CHECK_THROWS(extract_to(dest, files));                          // refuses to overwrite
    extract_to(dest, files, true);                                  // -f allows replacing regular files
    // symlinked directory component pointing outside
    fs::create_directory_symlink(tmp / "outside", tmp / "out" / "link");
    CHECK_THROWS(extract_to(dest, {{"link/evil.txt", Bytes{1}}}, true));
    CHECK(!fs::exists(tmp / "outside" / "evil.txt"));
    // final component is a symlink to an outside file
    fs::create_symlink(tmp / "outside" / "secret", tmp / "out" / "target");
    CHECK_THROWS(extract_to(dest, {{"target", Bytes{'X'}}}, true));
    CHECK(slurp(tmp / "outside" / "secret") == "SECRET");
    // regular file where a directory is needed
    { std::ofstream(tmp / "out" / "plain") << "x"; }
    CHECK_THROWS(extract_to(dest, {{"plain/inner", Bytes{1}}}, true));
    // unsafe paths are refused before anything is written
    CHECK_THROWS(extract_to(dest, {{"../escape", Bytes{1}}}));
    CHECK(!fs::exists(tmp / "escape"));
    fs::remove_all(tmp);
}

int main() {
    test_roundtrip_matrix();
    test_edge_sets();
    test_path_rules();
    test_limits();
    test_mutation_fuzz();
    test_extraction_safety();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
