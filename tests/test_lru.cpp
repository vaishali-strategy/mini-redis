#include <list>
#include <random>
#include <vector>

#include "check.hpp"
#include "lru.hpp"

struct Node {
    int id = 0;
    Node* lru_prev = nullptr;
    Node* lru_next = nullptr;
};

// Read the list front-to-back as ids.
static std::vector<int> order(const LruList<Node>& l) {
    std::vector<int> ids;
    for (Node* p = l.front(); p; p = p->lru_next) ids.push_back(p->id);
    return ids;
}

int main() {
    // Empty list.
    {
        LruList<Node> l;
        CHECK(l.empty() && l.size() == 0 && l.front() == nullptr && l.back() == nullptr);
        CHECK(l.pop_back() == nullptr);
        CHECK(l.validate());
    }

    // push_front puts the newest at the front, the oldest at the back.
    {
        LruList<Node> l;
        Node n[3];
        for (int i = 0; i < 3; ++i) {
            n[i].id = i;
            l.push_front(&n[i]);
        }
        CHECK((order(l) == std::vector<int>{2, 1, 0}));
        CHECK(l.back() == &n[0] && l.front() == &n[2]);
        CHECK(l.validate());
    }

    // move_to_front from the back, the middle, and when already at the front.
    {
        LruList<Node> l;
        Node n[4];
        for (int i = 0; i < 4; ++i) {
            n[i].id = i;
            l.push_front(&n[i]);
        }  // 3 2 1 0
        l.move_to_front(&n[0]);  // back -> front
        CHECK((order(l) == std::vector<int>{0, 3, 2, 1}) && l.validate());
        l.move_to_front(&n[2]);  // middle -> front
        CHECK((order(l) == std::vector<int>{2, 0, 3, 1}) && l.validate());
        l.move_to_front(&n[2]);  // already front: no change
        CHECK((order(l) == std::vector<int>{2, 0, 3, 1}) && l.validate());
        CHECK(l.back() == &n[1]);
    }

    // erase head, middle and tail.
    {
        LruList<Node> l;
        Node n[5];
        for (int i = 0; i < 5; ++i) {
            n[i].id = i;
            l.push_front(&n[i]);
        }  // 4 3 2 1 0
        l.erase(&n[4]);  // head
        CHECK((order(l) == std::vector<int>{3, 2, 1, 0}) && l.validate());
        l.erase(&n[1]);  // middle
        CHECK((order(l) == std::vector<int>{3, 2, 0}) && l.validate());
        l.erase(&n[0]);  // tail
        CHECK((order(l) == std::vector<int>{3, 2}) && l.validate());
        CHECK(l.back() == &n[2]);
        CHECK(n[0].lru_prev == nullptr && n[0].lru_next == nullptr);  // erased node is clean
    }

    // A one-element list: pop_back and erase empty it.
    {
        LruList<Node> l;
        Node a;
        l.push_front(&a);
        CHECK(l.front() == &a && l.back() == &a && l.validate());
        CHECK(l.pop_back() == &a);
        CHECK(l.empty() && l.front() == nullptr && l.back() == nullptr && l.validate());
        l.push_front(&a);
        l.erase(&a);
        CHECK(l.empty() && l.validate());
        l.push_front(&a);
        l.move_to_front(&a);  // single element
        CHECK(l.size() == 1 && l.validate());
    }

    // pop_back takes the least recently used.
    {
        LruList<Node> l;
        Node n[3];
        for (int i = 0; i < 3; ++i) {
            n[i].id = i;
            l.push_front(&n[i]);
        }
        l.move_to_front(&n[0]);  // 0 2 1
        CHECK(l.pop_back() == &n[1]);
        CHECK(l.pop_back() == &n[2]);
        CHECK(l.pop_back() == &n[0]);
        CHECK(l.empty());
    }

    // Randomised against std::list.
    {
        std::mt19937 rng(4242);
        LruList<Node> l;
        std::vector<Node> nodes(100);
        std::vector<bool> in(100, false);
        std::list<int> model;  // front = most recently used
        for (int i = 0; i < 100; ++i) nodes[i].id = i;

        for (int step = 0; step < 20000; ++step) {
            int i = static_cast<int>(rng() % 100);
            int op = static_cast<int>(rng() % 4);
            if (op == 0 && !in[i]) {
                l.push_front(&nodes[i]);
                model.push_front(i);
                in[i] = true;
            } else if (op == 1 && in[i]) {
                l.move_to_front(&nodes[i]);
                model.remove(i);
                model.push_front(i);
            } else if (op == 2 && in[i]) {
                l.erase(&nodes[i]);
                model.remove(i);
                in[i] = false;
            } else if (op == 3 && !model.empty()) {
                Node* t = l.pop_back();
                CHECK(t->id == model.back());
                in[t->id] = false;
                model.pop_back();
            }
            CHECK(l.size() == model.size());
            if (step % 25 == 0) {
                CHECK(l.validate());
                CHECK((order(l) == std::vector<int>(model.begin(), model.end())));
            }
        }
    }

    return finish("test_lru");
}
