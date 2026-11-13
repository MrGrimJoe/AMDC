#include "solid.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_set>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace amdc1 {
namespace solid {
namespace fs = std::filesystem;

namespace {

constexpr uint8_t kVersion = 1;
constexpr uint64_t kMaxFiles = uint64_t(1) << 22;
constexpr uint64_t kMaxGroup = 1024;
constexpr uint64_t kMaxUnitPages = uint64_t(1) << 20;
constexpr uint64_t kMinStripe = 4096, kMaxStripe = uint64_t(1) << 26;
constexpr size_t kMaxPath = 4096, kMaxComponent = 255;

enum StripeType : uint8_t { ST_RAW = 0, ST_LZ = 1, ST_FILL = 2 };

void put_varint(Bytes& o, uint64_t v) {
    while (v >= 0x80) { o.push_back(uint8_t((v & 0x7F) | 0x80)); v >>= 7; }
    o.push_back(uint8_t(v));
}
uint64_t get_varint(const uint8_t* b, size_t end, size_t& pos) {
    uint64_t v = 0;
    for (unsigned sh = 0;; sh += 7) {
        if (pos >= end || sh > 56) throw SolidError("bad varint");
        const uint8_t c = b[pos++];
        v |= uint64_t(c & 0x7F) << sh;
        if (!(c & 0x80)) return v;
    }
}
void put_u64(Bytes& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(uint8_t(v >> (8 * i))); }
uint64_t get_u64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i); return v; }

// ---- parallel helper: runs f(i) for i in [0,n); first exception wins and stops the rest ---------------------
template <class F>
void parallel_for(size_t n, unsigned threads, F&& f) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = unsigned(std::min<size_t>(threads, std::max<size_t>(1, n)));
    std::atomic<size_t> next{0};
    std::exception_ptr err;
    std::mutex m;
    auto worker = [&] {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= n) return;
            try { f(i); }
            catch (...) { std::lock_guard<std::mutex> g(m); if (!err) err = std::current_exception(); next.store(n); return; }
        }
    };
    if (threads <= 1) worker();
    else {
        std::vector<std::thread> th;
        for (unsigned t = 0; t < threads; ++t) th.emplace_back(worker);
        for (auto& t : th) t.join();
    }
    if (err) std::rethrow_exception(err);
}

// ---- layout: the virtual stream of a group ----------------------------------------------------------------
struct Seg { uint64_t file, off, len, vpos; };
struct Group {
    size_t first = 0, count = 0;
    std::vector<Seg> segs;
    uint64_t total = 0;
    size_t first_stripe = 0, nstripes = 0;
};
struct Stripe { size_t group; uint64_t vstart, vlen; };

// Round r takes unit r of every file of the group (in archive order); unit_pages==0 means one round of whole files.
void plan_group(const std::vector<uint64_t>& sizes, unsigned unit_pages, Group& g) {
    const uint64_t unit = unit_pages ? uint64_t(unit_pages) * kPage : 0;
    uint64_t vpos = 0;
    for (uint64_t round = 0;; ++round) {
        bool any = false;
        for (size_t k = 0; k < g.count; ++k) {
            const uint64_t sz = sizes[g.first + k];
            const uint64_t start = unit ? round * unit : 0;
            if (start >= sz) continue;
            const uint64_t len = unit ? std::min<uint64_t>(unit, sz - start) : sz;
            g.segs.push_back({g.first + k, start, len, vpos});
            vpos += len;
            any = true;
        }
        if (!any || !unit) break;
    }
    g.total = vpos;
}

void plan_all(const std::vector<uint64_t>& sizes, unsigned G, unsigned U, uint64_t SB, std::vector<Group>& groups,
              std::vector<Stripe>& stripes) {
    for (size_t first = 0; first < sizes.size(); first += G) {
        Group g;
        g.first = first;
        g.count = std::min<size_t>(G, sizes.size() - first);
        plan_group(sizes, U, g);
        g.nstripes = size_t((g.total + SB - 1) / SB);
        g.first_stripe = stripes.size();
        for (size_t s = 0; s < g.nstripes; ++s)
            stripes.push_back({groups.size(), uint64_t(s) * SB, std::min<uint64_t>(SB, g.total - uint64_t(s) * SB)});
        groups.push_back(std::move(g));
    }
}

// Calls f(seg, offset_inside_seg, offset_inside_stripe, n) for every piece of the virtual range [vs, vs+vl).
template <class F>
void for_pieces(const Group& g, uint64_t vs, uint64_t vl, F&& f) {
    auto it = std::upper_bound(g.segs.begin(), g.segs.end(), vs, [](uint64_t v, const Seg& s) { return v < s.vpos; });
    --it;
    uint64_t pos = vs;
    const uint64_t end = vs + vl;
    while (pos < end) {
        const uint64_t inner = pos - it->vpos, n = std::min(it->len - inner, end - pos);
        f(*it, inner, pos - vs, n);
        pos += n;
        ++it;
    }
}

bool all_same(const uint8_t* p, size_t n) {
    for (size_t i = 1; i < n; ++i) if (p[i] != p[0]) return false;
    return n > 0;
}

// Duplicate paths and file/directory conflicts ("a" and "a/b") make an archive ambiguous: reject both.
void check_paths(const std::vector<const std::string*>& paths) {
    std::unordered_set<std::string> seen, dirs;
    for (const std::string* p : paths) {
        if (!path_is_safe(*p)) throw SolidError("unsafe path in archive: " + *p);
        if (!seen.insert(*p).second) throw SolidError("duplicate path: " + *p);
        for (size_t i = 0; i < p->size(); ++i) if ((*p)[i] == '/') dirs.insert(p->substr(0, i));
    }
    for (const std::string* p : paths) if (dirs.count(*p)) throw SolidError("path is both a file and a directory: " + *p);
}

}  // namespace

bool path_is_safe(const std::string& p) {
    if (p.empty() || p.size() > kMaxPath) return false;
    size_t start = 0;
    for (size_t i = 0; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            const size_t len = i - start;
            if (len == 0 || len > kMaxComponent) return false;
            if (len == 1 && p[start] == '.') return false;
            if (len == 2 && p[start] == '.' && p[start + 1] == '.') return false;
            start = i + 1;
        } else {
            const unsigned char c = (unsigned char)p[i];
            if (c < 0x20 || c == 0x7F || c == '\\' || c == ':') return false;
        }
    }
    return true;
}

Bytes pack(const std::vector<File>& files, const Options& opt, bool validate) {
    if (opt.group < 1 || opt.group > kMaxGroup) throw SolidError("group must be 1..1024");
    if (opt.unit_pages > kMaxUnitPages) throw SolidError("unit_pages too large");
    if (opt.stripe_bytes < kMinStripe || opt.stripe_bytes > kMaxStripe) throw SolidError("stripe_bytes out of range");
    if (files.size() > kMaxFiles) throw SolidError("too many files");
    if (validate) {
        std::vector<const std::string*> ps;
        for (const File& f : files) ps.push_back(&f.path);
        check_paths(ps);
    }
    std::vector<uint64_t> sizes;
    for (const File& f : files) sizes.push_back(f.data.size());

    Bytes meta;
    for (const File& f : files) {
        put_varint(meta, f.path.size());
        meta.insert(meta.end(), f.path.begin(), f.path.end());
        put_varint(meta, f.data.size());
        put_u64(meta, fast::hash64(f.data.data(), f.data.size()));
    }
    Bytes meta_stored = meta;
    uint8_t meta_type = 0;
    if (!meta.empty()) {
        Bytes c(fast::block_bound(meta.size()));
        c.resize(fast::block_compress(meta.data(), meta.size(), c.data(), 2));
        if (c.size() < meta.size()) { meta_stored = std::move(c); meta_type = 1; }
    }

    std::vector<Group> groups;
    std::vector<Stripe> stripes;
    plan_all(sizes, opt.group, opt.unit_pages, opt.stripe_bytes, groups, stripes);

    std::vector<Bytes> enc(stripes.size());
    std::vector<uint8_t> type(stripes.size());
    parallel_for(stripes.size(), opt.threads, [&](size_t i) {
        const Stripe& s = stripes[i];
        const Group& g = groups[s.group];
        Bytes tmp(s.vlen);
        for_pieces(g, s.vstart, s.vlen, [&](const Seg& seg, uint64_t inner, uint64_t at, uint64_t n) {
            std::memcpy(tmp.data() + at, files[seg.file].data.data() + seg.off + inner, n);
        });
        if (all_same(tmp.data(), tmp.size())) { type[i] = ST_FILL; enc[i] = Bytes{tmp[0]}; return; }
        Bytes c(fast::block_bound(tmp.size()));
        c.resize(fast::block_compress(tmp.data(), tmp.size(), c.data(), opt.level));
        if (c.size() < tmp.size()) { type[i] = ST_LZ; enc[i] = std::move(c); }
        else { type[i] = ST_RAW; enc[i] = std::move(tmp); }
    });

    Bytes out{'A', 'M', 'D', 'S', kVersion, 0};
    put_varint(out, opt.group);
    put_varint(out, opt.unit_pages);
    put_varint(out, opt.stripe_bytes);
    put_varint(out, files.size());
    put_varint(out, meta.size());
    put_varint(out, meta_stored.size());
    out.push_back(meta_type);
    out.insert(out.end(), meta_stored.begin(), meta_stored.end());
    for (size_t i = 0; i < enc.size(); ++i) { out.push_back(type[i]); put_varint(out, enc[i].size()); }
    for (const Bytes& e : enc) out.insert(out.end(), e.begin(), e.end());
    put_u64(out, fast::hash64(out.data(), out.size()));
    return out;
}

std::vector<File> unpack(const uint8_t* in, size_t n, uint64_t max_output, unsigned threads) {
    if (n < 6 + 8 || std::memcmp(in, "AMDS", 4) != 0) throw SolidError("not an AMDS archive");
    if (in[4] != kVersion) throw SolidError("unsupported version");
    if (in[5] != 0) throw SolidError("unknown flags");
    const size_t end = n - 8;
    if (fast::hash64(in, end) != get_u64(in + end)) throw SolidError("archive checksum mismatch");

    size_t pos = 6;
    const uint64_t G = get_varint(in, end, pos), U = get_varint(in, end, pos), SB = get_varint(in, end, pos);
    const uint64_t nfiles = get_varint(in, end, pos), meta_raw = get_varint(in, end, pos),
                   meta_stored = get_varint(in, end, pos);
    if (G < 1 || G > kMaxGroup || U > kMaxUnitPages || SB < kMinStripe || SB > kMaxStripe || nfiles > kMaxFiles)
        throw SolidError("header parameters out of range");
    if (pos >= end) throw SolidError("truncated header");
    const uint8_t meta_type = in[pos++];
    if (meta_type > 1 || meta_stored > end - pos || meta_raw > (uint64_t(1) << 30)) throw SolidError("bad metadata size");
    Bytes meta;
    if (meta_type == 0) {
        if (meta_stored != meta_raw) throw SolidError("bad metadata size");
        meta.assign(in + pos, in + pos + meta_stored);
    } else {
        if (meta_stored < 1 || meta_raw > (meta_stored + 16) * 255) throw SolidError("implausible metadata ratio");
        meta.resize(meta_raw);
        if (!fast::block_decompress(in + pos, meta_stored, meta.data(), meta_raw)) throw SolidError("bad metadata block");
    }
    pos += meta_stored;

    std::vector<File> out(nfiles);
    std::vector<uint64_t> sizes(nfiles), hashes(nfiles);
    {
        size_t mp = 0;
        uint64_t total = 0;
        for (uint64_t i = 0; i < nfiles; ++i) {
            const uint64_t plen = get_varint(meta.data(), meta.size(), mp);
            if (plen == 0 || plen > kMaxPath || plen > meta.size() - mp) throw SolidError("bad path length");
            out[i].path.assign(reinterpret_cast<const char*>(meta.data()) + mp, plen);
            mp += plen;
            sizes[i] = get_varint(meta.data(), meta.size(), mp);
            if (meta.size() - mp < 8) throw SolidError("truncated metadata");
            hashes[i] = get_u64(meta.data() + mp);
            mp += 8;
            if (sizes[i] > max_output || total > max_output - sizes[i]) throw SolidError("declared size exceeds limit");
            total += sizes[i];
        }
        if (mp != meta.size()) throw SolidError("trailing metadata");
    }
    {
        std::vector<const std::string*> ps;
        for (const File& f : out) ps.push_back(&f.path);
        check_paths(ps);
    }

    std::vector<Group> groups;
    std::vector<Stripe> stripes;
    plan_all(sizes, unsigned(G), unsigned(U), SB, groups, stripes);
    if (stripes.size() > (end - pos) / 2) throw SolidError("truncated stripe directory");

    std::vector<uint8_t> type(stripes.size());
    std::vector<uint64_t> clen(stripes.size()), off(stripes.size());
    uint64_t sum = 0;
    for (size_t i = 0; i < stripes.size(); ++i) {
        if (pos >= end) throw SolidError("truncated stripe directory");
        type[i] = in[pos++];
        clen[i] = get_varint(in, end, pos);
        const uint64_t vl = stripes[i].vlen;
        if (type[i] == ST_RAW) { if (clen[i] != vl) throw SolidError("bad raw stripe"); }
        else if (type[i] == ST_FILL) { if (clen[i] != 1) throw SolidError("bad fill stripe"); }
        else if (type[i] == ST_LZ) { if (clen[i] < 1 || vl > (clen[i] + 16) * 255) throw SolidError("implausible stripe ratio"); }
        else throw SolidError("unknown stripe type");
        if (clen[i] > end) throw SolidError("bad stripe length");
        off[i] = sum;
        sum += clen[i];
        if (sum > end) throw SolidError("stripe data exceeds archive");
    }
    if (sum != end - pos) throw SolidError("stripe data size mismatch");
    const size_t data_start = pos;

    for (uint64_t i = 0; i < nfiles; ++i) out[i].data.assign(sizes[i], 0);
    parallel_for(stripes.size(), threads, [&](size_t i) {
        const Stripe& s = stripes[i];
        Bytes tmp(s.vlen);
        const uint8_t* src = in + data_start + off[i];
        if (type[i] == ST_RAW) std::memcpy(tmp.data(), src, s.vlen);
        else if (type[i] == ST_FILL) std::memset(tmp.data(), src[0], s.vlen);
        else if (!fast::block_decompress(src, size_t(clen[i]), tmp.data(), size_t(s.vlen))) throw SolidError("corrupt stripe");
        for_pieces(groups[s.group], s.vstart, s.vlen, [&](const Seg& seg, uint64_t inner, uint64_t at, uint64_t len) {
            std::memcpy(out[seg.file].data.data() + seg.off + inner, tmp.data() + at, len);
        });
    });
    parallel_for(nfiles, threads, [&](size_t i) {
        if (fast::hash64(out[i].data.data(), out[i].data.size()) != hashes[i]) throw SolidError("file checksum mismatch: " + out[i].path);
    });
    return out;
}

void extract_to(const std::string& dir, const std::vector<File>& files, bool force) {
    {
        std::vector<const std::string*> ps;
        for (const File& f : files) ps.push_back(&f.path);
        check_paths(ps);
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw SolidError("cannot create " + dir + ": " + ec.message());
    const fs::path root = fs::canonical(dir, ec);
    if (ec) throw SolidError("cannot resolve " + dir);
    for (const File& f : files) {
        fs::path cur = root;
        size_t start = 0;
        for (;;) {
            const size_t slash = f.path.find('/', start);
            const bool last = slash == std::string::npos;
            cur /= f.path.substr(start, last ? std::string::npos : slash - start);
            const fs::file_status st = fs::symlink_status(cur, ec);
            if (last) {
                if (fs::exists(st)) {
                    if (!force) throw SolidError("refusing to overwrite existing: " + f.path);
                    if (!fs::is_regular_file(st)) throw SolidError("refusing to overwrite non-file: " + f.path);
                }
                break;
            }
            if (fs::exists(st)) {
                if (fs::is_symlink(st) || !fs::is_directory(st)) throw SolidError("path component is a link or not a directory: " + f.path);
            } else {
                fs::create_directory(cur, ec);
                if (ec) throw SolidError("cannot create directory for " + f.path);
            }
            start = slash + 1;
        }
#ifndef _WIN32
        const int fd = ::open(cur.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | (force ? O_TRUNC : O_EXCL), 0644);
        if (fd < 0) throw SolidError("cannot create " + f.path);
        size_t done = 0;
        while (done < f.data.size()) {
            const ssize_t w = ::write(fd, f.data.data() + done, f.data.size() - done);
            if (w <= 0) { ::close(fd); throw SolidError("write failed: " + f.path); }
            done += size_t(w);
        }
        ::close(fd);
#else
        FILE* fp = std::fopen(cur.string().c_str(), force ? "wb" : "wbx");
        if (!fp) throw SolidError("cannot create " + f.path);
        const bool ok = f.data.empty() || std::fwrite(f.data.data(), 1, f.data.size(), fp) == f.data.size();
        std::fclose(fp);
        if (!ok) throw SolidError("write failed: " + f.path);
#endif
    }
}

}  // namespace solid
}  // namespace amdc1
