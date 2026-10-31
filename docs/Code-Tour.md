---
title: "Mini Redis: code tour and how to make it yours"
subtitle: "Read order, what to understand in each file, and the drills that turn this into a project you can defend"
---

Copy code from the `.md` files, not this `.docx`.

# 0. Read this first: why the code alone is not enough

The complete code was written for you in one session, and it works: every test passes, and
15 deliberate bugs were each caught by a test. That solves "get a working project". It does
**not** solve "defend it in an interview". SeatLock taught you that: you could not explain the
allocator in your own project.

An interviewer will not ask whether it works. They will ask:

- Why does the heap store positions?
- What happens if a key is deleted while it is still in the heap?
- What if one `write` sends only part of a reply?
- Why does `read` returning 0 not mean you can close immediately?

You can answer these only by having traced the code yourself. The four stages below take the
code from "someone else's" to "yours". Do them in order. Stage 4 is the one that matters.

# 1. The map of the project (11 source files, about 900 lines)

```
include/                       headers: what each part promises
  resp.hpp  heap.hpp  lru.hpp  store.hpp  commands.hpp  server.hpp
src/                           implementations
  resp.cpp                     bytes <-> commands and replies          (no sockets)
  store.cpp                    map + heap + list, kept consistent
  commands.cpp                 "GET a" -> store call -> reply          (no sockets)
  server.cpp                   sockets, poll() loop, per-client buffers
  main.cpp                     reads --port and --max-keys, starts the server
tests/
  test_resp.cpp  test_heap.cpp  test_lru.cpp  test_store.cpp   unit + randomised tests
  test_protocol.py                                             real server, real sockets
  bench.sh                                                     throughput numbers
```

`heap.hpp` and `lru.hpp` are header-only because they are templates: the whole implementation
lives in `include/`. The compiler is told where to find headers with `-Iinclude`.

Only `server.cpp` touches the network. Everything else is plain logic you can test without a
socket. That separation is a design point you should be able to explain.

# 2. Stage 1 - Read, in this order

Read each file once, slowly, with the questions beside it. Do not move on until you can
answer them. Write the answers down.

## `heap.hpp` (start here; it is self-contained)

- What does `heap_pos` do, and which operations would be impossible or slow without it?
- In `erase`, why swap the element with the last one before removing it?
- After that swap, why call `fix(i)` and not just `sift_down(i)`? Think of a case where the
  element moved into slot `i` is *smaller* than its new parent.
- What is the cost of `push`, `pop`, `erase`, `update`?
- Why does the heap store pointers (`T*`) and not copies of the entries?

## `lru.hpp`

- Why is the list *doubly* linked? What could you not do in O(1) with a singly linked one?
- Draw `erase` for three cases: the head, a middle node, the tail. Which `if` handles which?
- Why does `erase` set the node's `lru_prev`/`lru_next` to `nullptr` at the end?
- Why does LRU need both this list *and* the hash map?

## `store.hpp` / `store.cpp`

- One key lives in three structures. List every place a delete must reach. Which function does it?
- `Entry::key` is a pointer into the map. Why is it safe, and what C++ property of
  `unordered_map` does it depend on? What would break if the map stored its entries in a
  contiguous array that moves when it grows?
- Lazy versus active expiry: what does each one catch that the other does not?
- In `set`, why is the TTL erased from the heap when a key is overwritten?
- In `evict_if_needed`, why can the new key never be the one evicted?
- Why does `expire` call `heap_.update` for a key already in the heap instead of `push`?

## `resp.cpp`

- What are the three answers `parse_command` can give, and why must `NeedMore` and `Error`
  be different?
- Why check the declared length against the limit *before* waiting for the body?
- Why does the parser record spans first and copy strings only at the end?
- A bulk string can contain `\r\n`. How does the parser avoid mistaking that for the end?

## `commands.cpp`

- Why does the unknown-command error replace `\r` and `\n` in the name it echoes?
- Why is `DEL` allowed several keys but `GET` exactly one?

## `server.cpp` (read last; it ties everything together)

- Why are sockets non-blocking, including the listening one?
- Why is `POLLOUT` requested only when `out` is non-empty?
- What does `signal(SIGPIPE, SIG_IGN)` prevent?
- What is `closing` for? Trace what happens when a client sends a command and immediately
  half-closes, with a reply too big to send in one go.
- How does the heap's top decide the `poll()` timeout, and what happens when no key has a TTL?
- Why is the `fds` vector rebuilt every round?
- Why does `process_input` keep looping until `NeedMore`, and where is the unfinished tail kept?

# 3. Stage 2 - Run and watch

1. `make`, then `./server`, then in another terminal:
   ```bash
   redis-cli -p 6380 set a 1
   redis-cli -p 6380 expire a 3
   redis-cli -p 6380 ttl a
   sleep 4
   redis-cli -p 6380 dbsize        # 0, and you never asked for "a"
   ```
2. Start with `./server --max-keys 3`, set four keys, touch the first before adding the
   fourth, and predict which one is evicted *before* you check.
3. `make test`. Read one failing test's output by deliberately breaking something (stage 3).

# 4. Stage 3 - Break it on purpose

Each of these should make a test fail. For each: predict which test fails, make the change,
run `make test-unit` (or `make test-protocol`), and explain why that test caught it.
Undo the change afterwards.

| # | Change | Where |
|---|---|---|
| 1 | Delete the line `if (i != last) fix(i);` | `heap.hpp`, `erase` |
| 2 | Make `update` do nothing | `heap.hpp` |
| 3 | Delete `lru_.move_to_front(e);` in `get` | `store.cpp` |
| 4 | Delete the line that erases the TTL when a key is overwritten | `store.cpp`, `set` |
| 5 | Make `remove()` skip the heap | `store.cpp` |
| 6 | Evict `lru_.front()` instead of `back()` | `store.cpp` |
| 7 | Delete `store.expire_due();` | `server.cpp` |
| 8 | Delete `signal(SIGPIPE, SIG_IGN);` | `server.cpp` |
| 9 | Replace `c.out.erase(0, n)` with `c.out.clear()` | `server.cpp`, `write_to` |
| 10 | Remove the check that a bulk string is followed by `\r\n` | `resp.cpp` |

Then invent two more of your own and see whether the tests notice. If one slips through, you
have found a missing test: write it.

# 5. Stage 4 - Rebuild from a blank file (the one that makes it yours)

Do these over several days, one at a time. For each: **close this document**, delete the
file, and rewrite it from the *specification* (the header comment and the questions above),
not from memory of the code. Then run the tests. The tests are your examiner.

1. `lru.hpp` - easiest. Target: `make test-unit` passes `test_lru`.
2. `heap.hpp` - target: `test_heap` passes. The interesting part is `erase` and `fix`.
3. `resp.cpp` - target: `test_resp` passes.
4. `store.cpp` - target: `test_store` passes, including the 180,000-operation comparison.
5. `server.cpp` - hardest. Target: `make test-protocol` passes.

Keep the originals in git (`git stash` or a copy) so you can compare. Where yours differs and
still passes, note why. Where it fails, the failing test names the missing idea.

You are done when you can rewrite `heap.hpp` and `lru.hpp` with the tests passing and explain
every line to someone else, and sketch `server.cpp`'s loop on paper from scratch.

# 6. Questions to answer without notes (the interview set)

1. Walk one `SET greeting hello` from the bytes arriving on the socket to the reply leaving it.
2. Why one thread? What does that buy you, and what does it cost?
3. What does TCP guarantee, and what does it not? How does the server cope with the second part?
4. What is an indexed heap, and why did a plain priority queue not work?
5. Three structures hold one key. What goes wrong if you remove it from only two?
6. What is the difference between lazy and active expiry, and why have both?
7. How does the server survive a client that disconnects in the middle of a command?
8. What stops a malicious client from exhausting the server's memory? (Name every limit.)
9. Your benchmark shows no gain from 10 to 50 clients. Why? What would you try next?
10. If you needed to survive a restart, what would you add first?

# 7. What may honestly go on a resume

You may claim a thing only when you can defend it at stage 4. Until you have rebuilt the heap
and list yourself, the accurate phrase is "implemented with an indexed min-heap and an LRU
list" - which is true of the project - but you must be ready for the follow-up questions in
section 6. The line "hand-written" is true of the code (no library heap or list is used). Do
not suggest the design was entirely your own idea unless it is; say what you can explain.
