#pragma once
// Intrusive doubly linked list for LRU (least recently used) ordering.
//
// "Intrusive" means the prev/next pointers live inside the element itself, so given a
// pointer to an element we can unlink it in O(1) with no search. The front is the most
// recently used element, the back is the least recently used (the next one to evict).
//
// The list does not own its elements.
// T must have:   T* lru_prev = nullptr;   T* lru_next = nullptr;

#include <cstddef>

template <class T>
class LruList {
public:
    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }
    T* front() const { return head_; }  // most recently used (or nullptr)
    T* back() const { return tail_; }   // least recently used (or nullptr)

    // Add a new element at the front. t must not already be in the list.
    void push_front(T* t) {
        t->lru_prev = nullptr;
        t->lru_next = head_;
        if (head_)
            head_->lru_prev = t;
        else
            tail_ = t;  // list was empty: t is also the tail
        head_ = t;
        ++size_;
    }

    // Unlink t from wherever it is. Precondition: t is in the list.
    void erase(T* t) {
        if (t->lru_prev)
            t->lru_prev->lru_next = t->lru_next;
        else
            head_ = t->lru_next;  // t was the head
        if (t->lru_next)
            t->lru_next->lru_prev = t->lru_prev;
        else
            tail_ = t->lru_prev;  // t was the tail
        t->lru_prev = nullptr;
        t->lru_next = nullptr;
        --size_;
    }

    // Mark t as just used.
    void move_to_front(T* t) {
        if (head_ == t) return;
        erase(t);
        push_front(t);
    }

    // Remove and return the least recently used element, or nullptr if empty.
    T* pop_back() {
        T* t = tail_;
        if (t) erase(t);
        return t;
    }

    // Check the list is well formed. For tests.
    bool validate() const {
        size_t count = 0;
        T* prev = nullptr;
        for (T* p = head_; p; p = p->lru_next) {
            if (p->lru_prev != prev) return false;
            prev = p;
            if (++count > size_) return false;  // too long: a cycle or a wrong size_
        }
        return count == size_ && prev == tail_;
    }

private:
    T* head_ = nullptr;
    T* tail_ = nullptr;
    size_t size_ = 0;
};
