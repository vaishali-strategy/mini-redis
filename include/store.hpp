#pragma once
// The key-value store: a hash map plus two structures that point back into it.
//
//   unordered_map<key, Entry>   find a key in O(1)
//   ExpiryHeap                  which key expires next (keys that have a TTL only)
//   LruList                     which key was used least recently (every key)
//
// One key's Entry sits in the map AND (if it has a TTL) in the heap AND in the LRU
// list at the same time. Every way of removing a key (DEL, expiry, eviction) must
// unlink it from all of them; that is what Store::remove() is for.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "heap.hpp"
#include "lru.hpp"

struct Entry {
    std::string value;
    const std::string* key = nullptr;  // points at the map's own copy of the key
    int64_t expire_at = 0;             // absolute time in ms; meaningful only if in the heap
    size_t heap_pos = kNotInHeap;      // position in the heap; kNotInHeap = no TTL
    Entry* lru_prev = nullptr;
    Entry* lru_next = nullptr;
};

// Milliseconds on a monotonic clock. Tests pass a fake one.
using Clock = std::function<int64_t()>;
int64_t steady_now_ms();

constexpr int64_t kMaxTtlSeconds = 315360000;  // 10 years; keeps seconds*1000 far from overflow

class Store {
public:
    // max_keys == 0 means no limit.
    explicit Store(size_t max_keys = 0, Clock clock = steady_now_ms);

    // Create or overwrite. Overwriting clears any TTL. Evicts the least recently used
    // key if the store is now over its limit.
    void set(const std::string& key, std::string value);

    // nullptr if missing or expired. The pointer is valid until the next call that
    // changes the store.
    const std::string* get(const std::string& key);

    // true if the key existed.
    bool del(const std::string& key);

    // Set a TTL. seconds <= 0 deletes the key. false if the key does not exist.
    bool expire(const std::string& key, int64_t seconds);

    // -2 = no such key, -1 = key has no TTL, otherwise remaining seconds (rounded).
    int64_t ttl(const std::string& key);

    // Delete every key whose TTL has passed. Returns how many were removed.
    size_t expire_due();

    // When the earliest TTL fires (ms on the store's clock), if any key has a TTL.
    std::optional<int64_t> next_expiry() const;

    int64_t now() const { return clock_(); }
    size_t size() const { return map_.size(); }
    size_t ttl_count() const { return heap_.size(); }
    uint64_t evictions() const { return evictions_; }
    uint64_t expirations() const { return expirations_; }

    // Verify that the map, heap and LRU list agree with each other. For tests.
    bool check_invariants() const;

private:
    using Map = std::unordered_map<std::string, Entry>;

    Entry* find_live(const std::string& key);  // like find, but removes the key if it expired
    void remove(Map::iterator it);             // the one place a key is taken out of everything
    void evict_if_needed();

    Map map_;
    ExpiryHeap<Entry> heap_;
    LruList<Entry> lru_;
    size_t max_keys_;
    Clock clock_;
    uint64_t evictions_ = 0;
    uint64_t expirations_ = 0;
};
