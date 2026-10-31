#include "store.hpp"

#include <chrono>
#include <utility>

int64_t steady_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Store::Store(size_t max_keys, Clock clock) : max_keys_(max_keys), clock_(std::move(clock)) {}

// Take a key out of the map, the heap (if it has a TTL) and the LRU list.
void Store::remove(Map::iterator it) {
    Entry& e = it->second;
    if (e.heap_pos != kNotInHeap) heap_.erase(&e);
    lru_.erase(&e);
    map_.erase(it);
}

// Lazy expiry: a key whose TTL has passed is deleted the moment anyone looks for it,
// even if the active sweep (expire_due) has not reached it yet.
Entry* Store::find_live(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    Entry& e = it->second;
    if (e.heap_pos != kNotInHeap && e.expire_at <= clock_()) {
        remove(it);
        ++expirations_;
        return nullptr;
    }
    return &e;
}

void Store::evict_if_needed() {
    while (max_keys_ != 0 && map_.size() > max_keys_) {
        Entry* victim = lru_.back();
        remove(map_.find(*victim->key));
        ++evictions_;
    }
}

void Store::set(const std::string& key, std::string value) {
    if (Entry* e = find_live(key)) {
        e->value = std::move(value);
        if (e->heap_pos != kNotInHeap) heap_.erase(e);  // SET clears the TTL
        lru_.move_to_front(e);
        return;
    }
    auto [it, inserted] = map_.try_emplace(key);
    (void)inserted;
    Entry& e = it->second;
    e.value = std::move(value);
    e.key = &it->first;  // node-based map: this address stays valid until the key is erased
    lru_.push_front(&e);
    evict_if_needed();
}

const std::string* Store::get(const std::string& key) {
    Entry* e = find_live(key);
    if (!e) return nullptr;
    lru_.move_to_front(e);
    return &e->value;
}

bool Store::del(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    // A key whose TTL already passed counts as already gone.
    bool was_live = !(it->second.heap_pos != kNotInHeap && it->second.expire_at <= clock_());
    if (!was_live) ++expirations_;
    remove(it);
    return was_live;
}

bool Store::expire(const std::string& key, int64_t seconds) {
    Entry* e = find_live(key);
    if (!e) return false;
    if (seconds <= 0) {
        remove(map_.find(key));
        return true;
    }
    e->expire_at = clock_() + seconds * 1000;
    if (e->heap_pos == kNotInHeap)
        heap_.push(e);
    else
        heap_.update(e);  // a second EXPIRE moves the existing heap entry, never adds another
    return true;
}

int64_t Store::ttl(const std::string& key) {
    Entry* e = find_live(key);
    if (!e) return -2;
    if (e->heap_pos == kNotInHeap) return -1;
    int64_t remaining_ms = e->expire_at - clock_();
    return (remaining_ms + 500) / 1000;
}

size_t Store::expire_due() {
    size_t removed = 0;
    int64_t now = clock_();
    while (!heap_.empty() && heap_.top()->expire_at <= now) {
        Entry* e = heap_.top();
        remove(map_.find(*e->key));
        ++expirations_;
        ++removed;
    }
    return removed;
}

std::optional<int64_t> Store::next_expiry() const {
    if (heap_.empty()) return std::nullopt;
    return heap_.top()->expire_at;
}

bool Store::check_invariants() const {
    if (!lru_.validate() || !heap_.validate()) return false;
    if (lru_.size() != map_.size()) return false;

    size_t with_ttl = 0;
    for (const auto& [k, e] : map_) {
        if (e.key != &k) return false;
        if (e.heap_pos != kNotInHeap) ++with_ttl;
    }
    if (with_ttl != heap_.size()) return false;

    // Every element reachable from the LRU list must be the map's own entry for its key.
    for (const Entry* p = lru_.front(); p; p = p->lru_next) {
        auto it = map_.find(*p->key);
        if (it == map_.end() || &it->second != p) return false;
    }
    if (max_keys_ != 0 && map_.size() > max_keys_) return false;
    return true;
}
