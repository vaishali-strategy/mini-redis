#include <algorithm>
#include <random>
#include <set>
#include <vector>

#include "check.hpp"
#include "heap.hpp"

struct Item {
    int64_t expire_at = 0;
    size_t heap_pos = kNotInHeap;
    int id = 0;
};

int main() {
    // Pops come out in order.
    {
        ExpiryHeap<Item> h;
        std::vector<Item> items(8);
        int64_t times[] = {50, 10, 40, 20, 80, 30, 70, 60};
        for (int i = 0; i < 8; ++i) {
            items[i].expire_at = times[i];
            h.push(&items[i]);
            CHECK(h.validate());
        }
        CHECK(h.size() == 8);
        int64_t last = -1;
        while (!h.empty()) {
            Item* t = h.pop();
            CHECK(t->expire_at >= last);
            CHECK(t->heap_pos == kNotInHeap);
            last = t->expire_at;
            CHECK(h.validate());
        }
    }

    // Ties are allowed.
    {
        ExpiryHeap<Item> h;
        std::vector<Item> items(5);
        for (auto& it : items) {
            it.expire_at = 7;
            h.push(&it);
        }
        CHECK(h.validate());
        for (int i = 0; i < 5; ++i) CHECK(h.pop()->expire_at == 7);
    }

    // Erase from the root, the middle and the last slot.
    {
        ExpiryHeap<Item> h;
        std::vector<Item> items(10);
        for (int i = 0; i < 10; ++i) {
            items[i].expire_at = (i * 37) % 11;
            h.push(&items[i]);
        }
        h.erase(h.top());
        CHECK(h.validate() && h.size() == 9);
        for (int round = 0; round < 3; ++round) {
            // pick some element that is still in the heap
            Item* victim = nullptr;
            for (auto& it : items)
                if (h.contains(&it)) victim = &it;
            h.erase(victim);
            CHECK(!h.contains(victim));
            CHECK(h.validate());
        }
        CHECK(h.size() == 6);
    }

    // Update to an earlier and a later time.
    {
        ExpiryHeap<Item> h;
        std::vector<Item> items(6);
        for (int i = 0; i < 6; ++i) {
            items[i].expire_at = 100 + i * 10;
            h.push(&items[i]);
        }
        items[5].expire_at = 1;  // was the latest, now the earliest
        h.update(&items[5]);
        CHECK(h.validate() && h.top() == &items[5]);
        items[5].expire_at = 1000;  // now the latest
        h.update(&items[5]);
        CHECK(h.validate() && h.top() == &items[0]);
    }

    // One element: erase leaves it empty.
    {
        ExpiryHeap<Item> h;
        Item a;
        a.expire_at = 5;
        h.push(&a);
        h.erase(&a);
        CHECK(h.empty() && h.validate() && !h.contains(&a));
    }

    // Randomised: the heap must always agree with a std::multiset of the same elements.
    {
        std::mt19937 rng(777);
        ExpiryHeap<Item> h;
        std::vector<Item> items(200);
        for (size_t i = 0; i < items.size(); ++i) items[i].id = static_cast<int>(i);
        std::multiset<int64_t> model;

        for (int step = 0; step < 20000; ++step) {
            Item& it = items[rng() % items.size()];
            int op = static_cast<int>(rng() % 4);
            if (op == 0 && !h.contains(&it)) {  // push
                it.expire_at = static_cast<int64_t>(rng() % 500);
                h.push(&it);
                model.insert(it.expire_at);
            } else if (op == 1 && h.contains(&it)) {  // erase
                model.erase(model.find(it.expire_at));
                h.erase(&it);
            } else if (op == 2 && h.contains(&it)) {  // update
                model.erase(model.find(it.expire_at));
                it.expire_at = static_cast<int64_t>(rng() % 500);
                model.insert(it.expire_at);
                h.update(&it);
            } else if (op == 3 && !h.empty()) {  // pop
                Item* t = h.pop();
                CHECK(t->expire_at == *model.begin());
                model.erase(model.begin());
            }
            CHECK(h.size() == model.size());
            if (!h.empty()) CHECK(h.top()->expire_at == *model.begin());
            if (step % 50 == 0) CHECK(h.validate());
        }
        CHECK(h.validate());
    }

    return finish("test_heap");
}
