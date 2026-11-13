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

    // Stores a copy of `page` (page_size bytes). Compression and allocation happen outside the shard lock.
    size_t put(uint64_t id, const uint8_t* page) {
        std::unique_ptr<uint8_t[]> tmp(new uint8_t[page_encode_bound(page_)]);
        const size_t n = page_compress(page, page_, tmp.get(), level_, max_pct_);

        auto blob = std::make_shared<Blob>();
        if (n == 0) {
            blob->len = page_ + 1;
            blob->data.reset(new uint8_t[blob->len]);
            blob->data[0] = PAGE_RAW;
            std::memcpy(blob->data.get() + 1, page, page_);
        } else {
            blob->len = n;
            blob->data.reset(new uint8_t[n]);
            std::memcpy(blob->data.get(), tmp.get(), n);
        }

        const size_t used = blob->len;
        Shard& s = shard(id);
        std::shared_ptr<const Blob> retired;
        {
            std::lock_guard<std::mutex> g(s.m);
            auto it = s.map.find(id);
            if (it != s.map.end()) {
                bytes_.fetch_sub(it->second->len, std::memory_order_relaxed);
                retired = std::move(it->second);
                it->second = std::move(blob);
            } else {
                s.map.emplace(id, std::move(blob));
            }
            bytes_.fetch_add(used, std::memory_order_relaxed);
        }
        // Destroy an overwritten blob after releasing the shard lock.
        return used;
    }

    // Snapshot the immutable compressed blob under lock; decode after releasing the lock.
    // A concurrent put/erase cannot invalidate the snapshot.
    bool get(uint64_t id, uint8_t* out) const {
        const Shard& s = shard(id);
        std::shared_ptr<const Blob> snapshot;
        {
            std::lock_guard<std::mutex> g(s.m);
            auto it = s.map.find(id);
            if (it == s.map.end()) return false;
            snapshot = it->second;
        }
        return page_decompress(snapshot->data.get(), snapshot->len, out, page_);
    }

    bool erase(uint64_t id) {
        Shard& s = shard(id);
        std::shared_ptr<const Blob> retired;
        {
            std::lock_guard<std::mutex> g(s.m);
            auto it = s.map.find(id);
            if (it == s.map.end()) return false;
            bytes_.fetch_sub(it->second->len, std::memory_order_relaxed);
            retired = std::move(it->second);
            s.map.erase(it);
        }
        return true;
    }

    size_t page_size() const { return page_; }
    size_t compressed_bytes() const { return bytes_.load(std::memory_order_relaxed); }

private:
    struct Blob { std::unique_ptr<uint8_t[]> data; size_t len = 0; };
    struct Shard { mutable std::mutex m; std::unordered_map<uint64_t, std::shared_ptr<const Blob>> map; };
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
