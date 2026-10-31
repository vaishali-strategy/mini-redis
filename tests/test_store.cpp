#include <list>
#include <map>
#include <optional>
#include <random>
#include <string>

#include "check.hpp"
#include "commands.hpp"
#include "resp.hpp"
#include "store.hpp"

// A clock the test controls.
struct FakeClock {
    int64_t t = 1000;
    Clock fn() {
        return [this] { return t; };
    }
};

static std::string run(Store& s, std::initializer_list<std::string> args) {
    std::string out;
    execute_command(s, std::vector<std::string>(args), out);
    return out;
}

static void basics() {
    FakeClock c;
    Store s(0, c.fn());

    CHECK(s.get("a") == nullptr);
    s.set("a", "1");
    CHECK(s.get("a") && *s.get("a") == "1");
    s.set("a", "2");  // overwrite
    CHECK(*s.get("a") == "2" && s.size() == 1);
    CHECK(s.del("a"));
    CHECK(!s.del("a"));
    CHECK(s.size() == 0);
    CHECK(s.check_invariants());
}

static void ttl_rules() {
    FakeClock c;
    Store s(0, c.fn());

    CHECK(!s.expire("missing", 5));       // EXPIRE on a missing key
    CHECK(s.ttl("missing") == -2);

    s.set("k", "v");
    CHECK(s.ttl("k") == -1);              // exists, no TTL
    CHECK(s.expire("k", 10));
    CHECK(s.ttl("k") == 10);
    CHECK(s.ttl_count() == 1);

    CHECK(s.expire("k", 20));             // second EXPIRE moves the entry, never duplicates it
    CHECK(s.ttl_count() == 1 && s.ttl("k") == 20);
    CHECK(s.expire("k", 5));              // and can shorten it
    CHECK(s.ttl_count() == 1 && s.ttl("k") == 5);
    CHECK(s.check_invariants());

    c.t += 2000;
    CHECK(s.ttl("k") == 3);

    s.set("k", "new");                    // SET clears the TTL
    CHECK(s.ttl("k") == -1 && s.ttl_count() == 0);
    CHECK(s.check_invariants());

    CHECK(s.expire("k", 5));
    CHECK(s.del("k"));                    // DEL removes it from the heap too
    CHECK(s.ttl_count() == 0 && s.size() == 0);
    CHECK(s.check_invariants());

    s.set("z", "v");
    CHECK(s.expire("z", 0));              // non-positive seconds deletes
    CHECK(s.get("z") == nullptr && s.ttl_count() == 0);
    s.set("z", "v");
    CHECK(s.expire("z", -3));
    CHECK(s.get("z") == nullptr);
    CHECK(s.check_invariants());
}

static void expiry() {
    FakeClock c;
    Store s(0, c.fn());

    CHECK(!s.next_expiry().has_value());
    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    s.expire("a", 3);
    s.expire("b", 1);
    s.expire("c", 2);
    CHECK(s.next_expiry().value() == c.t + 1000);  // b is due first

    // Active expiry: nobody touches the keys, expire_due removes them anyway.
    c.t += 1000;
    CHECK(s.expire_due() == 1 && s.size() == 2);
    CHECK(s.next_expiry().value() == 1000 + 2000);
    c.t += 5000;
    CHECK(s.expire_due() == 2 && s.size() == 0 && s.ttl_count() == 0);
    CHECK(s.expirations() == 3);
    CHECK(s.check_invariants());

    // Lazy expiry: the key is overdue but expire_due has not run; any access removes it.
    s.set("x", "1");
    s.expire("x", 1);
    c.t += 1500;
    CHECK(s.size() == 1);            // still physically there
    CHECK(s.get("x") == nullptr);    // looking for it deletes it
    CHECK(s.size() == 0 && s.ttl_count() == 0);

    s.set("y", "1");
    s.expire("y", 1);
    c.t += 1500;
    CHECK(!s.del("y"));              // overdue key counts as already gone
    CHECK(s.size() == 0);
    CHECK(s.check_invariants());
}

static void eviction() {
    FakeClock c;
    Store s(3, c.fn());

    s.set("a", "1");
    s.set("b", "2");
    s.set("c", "3");
    s.get("a");        // a is now the most recently used; b is the oldest
    s.set("d", "4");   // over the limit: evict b
    CHECK(s.size() == 3);
    CHECK(s.get("b") == nullptr);
    CHECK(s.get("a") && s.get("c") && s.get("d"));
    CHECK(s.evictions() == 1);

    // Overwriting counts as use.
    s.set("a", "9");   // order now: a d c ... (c oldest after the gets above)
    s.set("e", "5");
    CHECK(s.size() == 3 && s.get("a") != nullptr);
    CHECK(s.check_invariants());

    // An evicted key's TTL entry goes with it (a stale heap pointer would crash expire_due).
    Store t(2, c.fn());
    t.set("p", "1");
    t.expire("p", 1);
    t.set("q", "2");
    t.set("r", "3");   // evicts p
    CHECK(t.get("p") == nullptr && t.ttl_count() == 0);
    c.t += 5000;
    CHECK(t.expire_due() == 0);
    CHECK(t.check_invariants());

    // Limit of 1: the newest key always survives.
    Store u(1, c.fn());
    u.set("a", "1");
    u.set("b", "2");
    CHECK(u.size() == 1 && u.get("b") != nullptr && u.get("a") == nullptr);
    CHECK(u.check_invariants());
}

static void commands() {
    FakeClock c;
    Store s(0, c.fn());

    CHECK(run(s, {"PING"}) == "+PONG\r\n");
    CHECK(run(s, {"ping", "hi"}) == "$2\r\nhi\r\n");  // case-insensitive
    CHECK(run(s, {"SET", "a", "1"}) == "+OK\r\n");
    CHECK(run(s, {"GET", "a"}) == "$1\r\n1\r\n");
    CHECK(run(s, {"GET", "nope"}) == "$-1\r\n");
    CHECK(run(s, {"EXPIRE", "a", "10"}) == ":1\r\n");
    CHECK(run(s, {"EXPIRE", "nope", "10"}) == ":0\r\n");
    CHECK(run(s, {"TTL", "a"}) == ":10\r\n");
    CHECK(run(s, {"TTL", "nope"}) == ":-2\r\n");
    run(s, {"SET", "b", "2"});
    CHECK(run(s, {"TTL", "b"}) == ":-1\r\n");
    CHECK(run(s, {"DBSIZE"}) == ":2\r\n");
    CHECK(run(s, {"DEL", "a", "b", "zzz"}) == ":2\r\n");  // counts only keys that existed
    CHECK(run(s, {"DBSIZE"}) == ":0\r\n");

    CHECK(run(s, {"EXPIRE", "a", "abc"}).rfind("-ERR value is not an integer", 0) == 0);
    CHECK(run(s, {"EXPIRE", "a", "99999999999"}).rfind("-ERR invalid expire time", 0) == 0);
    CHECK(run(s, {"GET"}).rfind("-ERR wrong number of arguments for 'get'", 0) == 0);
    CHECK(run(s, {"SET", "a"}).rfind("-ERR wrong number", 0) == 0);
    CHECK(run(s, {"SET", "a", "b", "c"}).rfind("-ERR wrong number", 0) == 0);
    CHECK(run(s, {"NOPE"}) == "-ERR unknown command 'NOPE'\r\n");
    CHECK(run(s, {"NO\r\nPE"}) == "-ERR unknown command 'NO  PE'\r\n");  // no CRLF injected
}

// ---------------------------------------------------------------------------------------------
// Differential test: drive the real Store and a deliberately simple reference model with the
// same random operations and demand identical answers. The model uses std::list + std::map
// and none of the store's pointer tricks, so a bug in either shows up as a disagreement.
// ---------------------------------------------------------------------------------------------
struct Model {
    struct E {
        std::string value;
        std::optional<int64_t> exp;
    };
    size_t max_keys;
    int64_t* now;
    std::map<std::string, E> m;
    std::list<std::string> order;  // front = most recently used

    void drop(const std::string& k) {
        m.erase(k);
        order.remove(k);
    }
    bool live(const std::string& k) {
        auto it = m.find(k);
        if (it == m.end()) return false;
        if (it->second.exp && *it->second.exp <= *now) {
            drop(k);
            return false;
        }
        return true;
    }
    void touch(const std::string& k) {
        order.remove(k);
        order.push_front(k);
    }
    void set(const std::string& k, const std::string& v) {
        if (live(k)) {
            m[k].value = v;
            m[k].exp.reset();
            touch(k);
            return;
        }
        m[k] = E{v, std::nullopt};
        order.push_front(k);
        while (max_keys != 0 && m.size() > max_keys) drop(order.back());
    }
    const std::string* get(const std::string& k) {
        if (!live(k)) return nullptr;
        touch(k);
        return &m[k].value;
    }
    bool del(const std::string& k) {
        auto it = m.find(k);
        if (it == m.end()) return false;
        bool was_live = !(it->second.exp && *it->second.exp <= *now);
        drop(k);
        return was_live;
    }
    bool expire(const std::string& k, int64_t secs) {
        if (!live(k)) return false;
        if (secs <= 0) {
            drop(k);
            return true;
        }
        m[k].exp = *now + secs * 1000;
        return true;
    }
    int64_t ttl(const std::string& k) {
        if (!live(k)) return -2;
        if (!m[k].exp) return -1;
        return (*m[k].exp - *now + 500) / 1000;
    }
    size_t expire_due() {
        std::vector<std::string> due;
        for (auto& [k, e] : m)
            if (e.exp && *e.exp <= *now) due.push_back(k);
        for (auto& k : due) drop(k);
        return due.size();
    }
};

static void differential(size_t max_keys, unsigned seed) {
    FakeClock c;
    Store s(max_keys, c.fn());
    Model model{max_keys, &c.t, {}, {}};
    std::mt19937 rng(seed);
    auto key = [&] { return "k" + std::to_string(rng() % 25); };

    for (int step = 0; step < 30000; ++step) {
        std::string k = key();
        switch (rng() % 8) {
            case 0:
            case 1: {
                std::string v = "v" + std::to_string(step);
                s.set(k, v);
                model.set(k, v);
                break;
            }
            case 2: {
                const std::string* a = s.get(k);
                const std::string* b = model.get(k);
                CHECK((a == nullptr) == (b == nullptr));
                if (a && b) CHECK(*a == *b);
                break;
            }
            case 3:
                CHECK(s.del(k) == model.del(k));
                break;
            case 4: {
                int64_t secs = static_cast<int64_t>(rng() % 7) - 1;  // -1 .. 5
                CHECK(s.expire(k, secs) == model.expire(k, secs));
                break;
            }
            case 5:
                CHECK(s.ttl(k) == model.ttl(k));
                break;
            case 6:
                c.t += static_cast<int64_t>(rng() % 1800);
                CHECK(s.expire_due() == model.expire_due());
                break;
            case 7:
                c.t += static_cast<int64_t>(rng() % 400);  // time passes, no sweep: lazy expiry
                break;
        }
        CHECK(s.size() == model.m.size());
        if (step % 10 == 0) CHECK(s.check_invariants());
    }
    CHECK(s.check_invariants());
}

int main() {
    basics();
    ttl_rules();
    expiry();
    eviction();
    commands();
    differential(0, 1);
    differential(0, 2);
    differential(6, 3);
    differential(6, 4);
    differential(1, 5);
    differential(25, 6);
    return finish("test_store");
}
