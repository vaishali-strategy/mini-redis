#pragma once
// Indexed binary min-heap, ordered by `expire_at` (earliest first).
//
// A plain std::priority_queue can only look at / remove the top. We also need to
// remove or re-time an element that is somewhere in the middle (DEL, a second EXPIRE,
// LRU eviction). So every element remembers its own position in the heap array
// (`heap_pos`), which lets us find it in O(1) and then fix the heap in O(log n).
//
// The heap does not own its elements; it stores pointers.
// T must have:   int64_t expire_at;   size_t heap_pos = kNotInHeap;

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

constexpr size_t kNotInHeap = std::numeric_limits<size_t>::max();

template <class T>
class ExpiryHeap {
public:
    bool empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }

    // The element with the earliest expire_at. Precondition: !empty().
    T* top() const { return items_.front(); }

    bool contains(const T* t) const { return t->heap_pos != kNotInHeap; }

    // Insert t (its expire_at must already be set). O(log n).
    void push(T* t) {
        t->heap_pos = items_.size();
        items_.push_back(t);
        sift_up(t->heap_pos);
    }

    // Remove and return the earliest element. Precondition: !empty(). O(log n).
    T* pop() {
        T* t = items_.front();
        erase(t);
        return t;
    }

    // Remove t from wherever it is. Precondition: contains(t). O(log n).
    void erase(T* t) {
        size_t i = t->heap_pos;
        size_t last = items_.size() - 1;
        if (i != last) swap_at(i, last);  // t moves to the end, the old last takes slot i
        items_.pop_back();
        t->heap_pos = kNotInHeap;
        if (i != last) fix(i);  // the element now in slot i may belong higher or lower
    }

    // Call after changing t->expire_at while t is in the heap. O(log n).
    void update(T* t) { fix(t->heap_pos); }

    // Check every rule the heap relies on. For tests.
    bool validate() const {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i]->heap_pos != i) return false;
            if (i > 0 && items_[i]->expire_at < items_[(i - 1) / 2]->expire_at) return false;
        }
        return true;
    }

private:
    bool less(size_t a, size_t b) const { return items_[a]->expire_at < items_[b]->expire_at; }

    void swap_at(size_t a, size_t b) {
        T* tmp = items_[a];
        items_[a] = items_[b];
        items_[b] = tmp;
        items_[a]->heap_pos = a;
        items_[b]->heap_pos = b;
    }

    void sift_up(size_t i) {
        while (i > 0) {
            size_t parent = (i - 1) / 2;
            if (!less(i, parent)) break;
            swap_at(i, parent);
            i = parent;
        }
    }

    void sift_down(size_t i) {
        size_t n = items_.size();
        while (true) {
            size_t left = 2 * i + 1;
            size_t right = left + 1;
            size_t smallest = i;
            if (left < n && less(left, smallest)) smallest = left;
            if (right < n && less(right, smallest)) smallest = right;
            if (smallest == i) break;
            swap_at(i, smallest);
            i = smallest;
        }
    }

    // Slot i may now be too small (move up) or too big (move down).
    void fix(size_t i) {
        if (i > 0 && less(i, (i - 1) / 2))
            sift_up(i);
        else
            sift_down(i);
    }

    std::vector<T*> items_;
};
