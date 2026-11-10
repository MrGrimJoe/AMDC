// CompressedPageCache: keep pages compressed in RAM, decompress on access (a user-space take on
// what an OS memory compressor does). Header-only, thread-safe (sharded locks), no OS dependencies.
#pragma once
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "fast.hpp"

namespace amdc1 {
namespace fast {

class CompressedPageCache {
public:
    explicit CompressedPageCache(size_t page_size = 4096, int level = 1, unsigned max_pct = 75)
        : page_(page_size), level_(level), max_pct_(max_pct) {}

    // Stores a copy of `page` (page_size bytes). Returns the bytes of RAM used for it.
    size_t put(uint64_t id, const uint8_t* page) {
        std::unique_ptr<uint8_t[]> tmp(new uint8_t[page_encode_bound(page_)]);
        size_t n = page_compress(page, page_, tmp.get(), level_, max_pct_);
        Entry e;
        if (n == 0) {                       // incompressible: store raw with a 1-byte kind tag
            e.len = page_ + 1;
            e.data.reset(new uint8_t[e.len]);
            e.data[0] = PAGE_RAW;
            std::memcpy(e.data.get() + 1, page, page_);
        } else {
            e.len = n;
            e.data.reset(new uint8_t[n]);
            std::memcpy(e.data.get(), tmp.get(), n);
        }
        const size_t used = e.len;
        Shard& s = shard(id);
        std::lock_guard<std::mutex> g(s.m);
        auto it = s.map.find(id);
        if (it != s.map.end()) { bytes_ -= it->second.len; it->second = std::move(e); }
        else s.map.emplace(id, std::move(e));
        bytes_ += used;
        return used;
    }

    // Copies the page into `out` (page_size bytes). Returns false if the id is unknown.
    bool get(uint64_t id, uint8_t* out) const {
        const Shard& s = shard(id);
        std::lock_guard<std::mutex> g(s.m);
        auto it = s.map.find(id);
        if (it == s.map.end()) return false;
        return page_decompress(it->second.data.get(), it->second.len, out, page_);
    }

    bool erase(uint64_t id) {
        Shard& s = shard(id);
        std::lock_guard<std::mutex> g(s.m);
        auto it = s.map.find(id);
        if (it == s.map.end()) return false;
        bytes_ -= it->second.len;
        s.map.erase(it);
        return true;
    }

    size_t page_size() const { return page_; }
    size_t compressed_bytes() const { return bytes_; }

private:
    struct Entry { std::unique_ptr<uint8_t[]> data; size_t len = 0; };
    struct Shard { mutable std::mutex m; std::unordered_map<uint64_t, Entry> map; };
    Shard shards_[16];
    Shard& shard(uint64_t id) { return shards_[(id * 0x9E3779B97F4A7C15ull) >> 60]; }
    const Shard& shard(uint64_t id) const { return shards_[(id * 0x9E3779B97F4A7C15ull) >> 60]; }
    size_t page_;
    int level_;
    unsigned max_pct_;
    std::atomic<size_t> bytes_{0};
};

}  // namespace fast
}  // namespace amdc1
