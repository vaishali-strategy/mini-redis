# Mini Redis: 14-day plan

A single-threaded C++ in-memory key-value server that speaks RESP over TCP. Flagship resume project.
Written 29 Oct 2026. Target finish: about 12 Nov 2026 (about 3 hrs/day, about 40 hrs).

## Why this project, and why this scope (the reasoning, keep it)

- **Goal:** one project that shows she understands how systems work *and* can build it, with real
  DSA inside. Not a proof of concept in theory (route planner already is that), not a product.
- **Why not SeatLock as the flagship:** its core is atomics and cache lines; the data structure is a
  bitmap, so it shows concurrency, not DSA breadth. It stays as a strong second project.
- **Why not an LSM-tree store or search engine:** she asked for *simple*. Mini Redis is one process,
  one event loop, no disk.
- **Locked scope:** SET, GET, DEL, EXPIRE, TTL; LRU eviction by max key count; RESP; `poll()` event
  loop; `std::unordered_map` as the store; **she writes the min-heap and the LRU list**.
- **Rejected, and why:**
  - Own hash table: she judged it pointless without knowing what it's for. Still must be able to
    explain how `unordered_map` handles collisions and rehashing. Optional upgrade if she finishes early.
  - `epoll`: she is on macOS (`kqueue`, no epoll). `poll()` is enough for the scope.
  - Comparison against real Redis: `poll()` would lose at high client counts, and she chose to make
    no comparison claims. RESP and `redis-cli`/`redis-benchmark` are still named once, honestly.
  - Persistence, other data types, replication, multithreading, cloud: out of scope.
- **Sanitizers:** undecided. She decides on Day 14. If not run, the README makes no leak/crash claim.
- **Process rule:** **she types every line.** Claude gives design, order, test cases and review.
  Reason: SeatLock was written in a Claude session and she could not defend it afterwards.
  **Amended 29 Oct, Day 1:** she has never written a socket program, and nobody writes the OS-call
  boilerplate (`socket/bind/listen/accept`, later `poll`) from memory. So Claude gives a worked,
  line-by-line explained example for that boilerplate only (Days 1-2); she types it, understands
  each call, then changes it. The parser, heap, LRU list, command logic and tests are the project's
  real content and stay hers: Claude gives the design and checks, she writes the code.

## Change of plan, 30 Oct (Day 2): the complete project was built in one session

**What happened:** after typing Day 1 and Day 2 herself, she asked for "the complete version now"
and said she can learn from each piece. Claude wrote and verified the whole project:
RESP parser, indexed min-heap, LRU list, store, commands, poll() server, unit tests, protocol
tests, benchmark script, README. All tests pass, the same tests pass under ASan+UBSan, and 15
deliberate mutations were each caught by a test.

**Why this needs care:** the original rule (she types every line) existed because SeatLock was
Claude-written and she could not defend it. The risk is the same again. The 14-day plan below is
therefore **repurposed from "build" to "own"**:

- Days 3-5 now mean: read, run, break, explain (Code-Tour.md, stages 1-3).
- The remaining days become the **rebuild drills** (stage 4): she deletes a file and rewrites it
  from the spec until the tests pass: lru.hpp, heap.hpp, resp.cpp, store.cpp, server.cpp.
- The resume line stays a draft until she can explain heap.hpp, lru.hpp and the event loop
  unaided. The Day 14 defence questions still apply unchanged.
- Her day-1 and day-2 files are preserved in `archive/`.
- Sanitizer claim still undecided; `make asan` exists and she runs it herself before claiming anything.
- Benchmark numbers in the README are from 30 Oct on her Apple M2; she should re-run `make bench`
  herself before quoting them.

## Ground rules

1. Each day ends with the "done when" check passing. If not, the next day starts there.
2. Before moving on, answer that day's **defence questions** out loud or in writing, without notes.
   Send the answers to Claude to check.
3. Send each finished file to Claude to review before the next day starts.
4. Day 6 and Day 14 are checkpoints with slack. Use them to catch up rather than adding scope.
5. Never quote a benchmark number as a constant. Record the machine, command and date next to it.

## Layout

```
mini-redis/
  src/        main.cpp server.cpp resp.cpp store.cpp heap.hpp lru.hpp
  tests/      test_resp.cpp test_heap.cpp test_lru.cpp test_protocol.py
  Makefile
  README.md
  PLAN.md
```

Build with plain `make` and `clang++ -std=c++23 -Wall -Wextra -O2`. No CMake. Tests are small
`assert`-based programs; protocol tests are a stdlib-only Python socket script.

## Day 1: Sockets and a blocking server

- **Do:** `brew install redis` (only for `redis-cli` and `redis-benchmark`; you will never run
  `redis-server`). Write a blocking TCP server: `socket`, `bind`, `listen`, `accept`, `read`, `write`.
  It echoes what one client sends. Test with `nc localhost 6380`.
- **Done when:** two sequential `nc` sessions work, and the second works only after the first closes.
- **Defence questions:** What is a file descriptor? Why is TCP a byte stream with no message
  boundaries? What does `accept` return and how is it different from the listening socket?

## Day 2: `poll()` event loop, many clients

- **Do:** make sockets non-blocking. A `Client` struct per connection with `in` and `out` buffers.
  `poll()` over the listening socket plus all clients; accept, read, write only when ready.
  Handle `read` returning 0 (disconnect) and `EAGAIN`; close and free the client.
- **Done when:** 3 `nc` sessions are served at the same time, and killing one does not affect the others.
- **Defence questions:** Why one thread, many clients? What does `poll()` tell you and what does it not?
  Why must the listening socket be non-blocking too? What breaks if `write` sends only part of the reply?

## Day 3: Incremental RESP parser (standalone)

- **Do:** parse RESP arrays of bulk strings (`*3\r\n$3\r\nSET\r\n...`). The function takes a buffer
  and returns one of: a complete command (plus bytes consumed), NEED_MORE, or ERROR. Enforce caps:
  max bulk length, max argument count. Test it alone, with no sockets.
- **Done when:** `test_resp.cpp` passes: a full command, a command fed one byte at a time, two
  commands in one buffer, a negative length, an oversized length, garbage first byte.
- **Defence questions:** Why must the parser be resumable? How do you know where one command
  ends and the next begins? Why cap the length *before* allocating?

## Day 4: Commands and replies, pipelining

- **Do:** connect the parser to the loop. Loop `parse` until NEED_MORE so pipelined commands all
  run. Implement reply encoders (`+OK`, `$n` bulk, `$-1` nil, `:n` integer, `-ERR ...`).
  Implement PING, SET, GET, DEL on `std::unordered_map<std::string, Entry>`.
- **Done when:** `redis-cli -p 6380 set a 1`, `get a`, `del a`, `get a` give the right answers.
- **Defence questions:** What happens to leftover bytes after one command is consumed? How does a
  pipelined batch get replies back in order? What does `unordered_map` do on collision and on rehash?
  (Answer from your own understanding; Claude will check.)

## Day 5: Protocol tests and hardening

- **Do:** write `test_protocol.py`: byte-at-a-time send, 100 pipelined commands in one packet,
  malformed input, oversized length, client closes mid-command. On bad input reply `-ERR protocol
  error` and close that connection only. Fix what breaks.
- **Done when:** the protocol tests pass against a freshly started server, and the server keeps
  serving other clients after each bad one.
- **Defence questions:** Why close the connection after a protocol error rather than continue?
  What state belongs to a client and must be freed when it disconnects?

## Day 6: Checkpoint and catch-up (no new scope)

- **Do:** finish anything unfinished. Read the whole code once top to bottom. Write the first half
  of the README: what it is, how to run it, the command list.
- **Done when:** a stranger could clone it, run `make`, and use `redis-cli` against it.
- **Mid-point defence:** explain the path of one `SET` from socket bytes to the map and back, with
  no notes. Send it to Claude.

## Day 7: Indexed min-heap (standalone)

- **Do:** write a binary min-heap keyed by expiry time that also tracks each element's position, so
  you can `erase` and `update` an element in O(log n), not just pop the top. Draw the sift-up and
  sift-down on paper first. Test alone.
- **Done when:** `test_heap.cpp` passes: ordered pops, ties, erase from the middle, update to
  earlier and later times, and a randomised check against a sorted `std::vector`.
- **Defence questions:** Why is a plain `priority_queue` not enough here? Why store positions?
  Cost of push, pop-min, erase, update? Why is building a heap from n items O(n)?

## Day 8: EXPIRE and TTL

- **Do:** add `expire_at` to `Entry` and keep the heap in sync. `EXPIRE`/`TTL` commands.
  Lazy expiry: check on every access. Active expiry: use the heap top to compute the `poll()`
  timeout, and delete everything due when it fires.
- **Done when:** `SET k v`, `EXPIRE k 2`, `TTL k`, wait, `GET k` returns nil, without any command
  touching the key in between (active expiry works).
- **Defence questions:** What is the difference between lazy and active expiry, and why have both?
  How does the heap's top set the `poll()` timeout?

## Day 9: Expiry correctness

- **Do:** handle every interaction. `SET` on an existing key clears its TTL. `DEL` removes it from
  the heap. `EXPIRE` on a missing key. `EXPIRE` a second time updates, not duplicates. TTL of a
  key with no expiry vs missing key. Add tests for each.
- **Done when:** each case has a passing protocol test, and the heap size always equals the number
  of keys that have a TTL.
- **Defence questions:** How do the map and the heap stay consistent on `DEL`? What breaks if a
  key is deleted but its heap entry is left behind?

## Day 10: LRU list (standalone)

- **Do:** write a doubly linked list with `push_front`, `move_to_front`, `erase`, `pop_back`, all
  O(1) given a node pointer. Decide whether nodes live inside `Entry` or are separate, and write
  down why. Test alone.
- **Done when:** `test_lru.cpp` passes: ordering after touches, erase from head, middle and tail,
  pop from a one-element list, empty list.
- **Defence questions:** Why doubly linked? Why does LRU need the map as well as the list?
  What are head and tail edge cases that cause pointer bugs?

## Day 11: LRU eviction

- **Do:** a `--max-keys N` option. Every `GET`/`SET` moves the key to the front. When the count
  exceeds the limit, evict from the tail: remove from the map, the list and the heap.
- **Done when:** with `--max-keys 3`, setting 4 keys evicts the least recently *used* (touch `a`
  first and `b` goes instead), and an evicted key's TTL entry is gone.
- **Defence questions:** One key lives in three structures. List every place a delete must touch.
  Is expiry or eviction removal different from `DEL`? Where would you put the shared code?

## Day 12: Consistency tests

- **Do:** write a randomised test: apply thousands of random SET/GET/DEL/EXPIRE operations to your
  server and to a trivial reference model (a `std::map` plus a fake clock), and compare replies.
  After each batch, check invariants: map size, heap size vs TTL keys, list size equals map size.
- **Done when:** it passes for many seeds. Fix every mismatch; each one is a bug you found.
- **Defence questions:** What invariants tie the three structures together? What was the worst bug
  this found?

## Day 13: Benchmark

- **Do:** run `redis-benchmark -p 6380 -t set,get -n 100000 -c 1`, then `-c 10`, then `-c 50`, plus
  one run with `-P 16` for pipelining. Record machine, date, build flags and exact commands.
  Make no comparisons to anything else.
- **Done when:** a README table of ops/sec with the method beside it, from at least three runs per
  row so you can say how much they vary.
- **Defence questions:** Why does throughput change with client count? What does pipelining change
  and why? What is the bottleneck of a single-threaded `poll()` loop, and where would you look first?

## Day 14: Defence day and finish

- **Do:** finish the README (design, data structures and why, limitations, how to test). Decide on
  sanitizers: run ASan and UBSan on the unit and protocol tests, or remove all leak/crash claims.
  Final read-through. Commit.
- **Done when:** the README states only what you have verified.
- **Final defence:** with no notes, explain (a) the event loop, (b) the three structures and how
  they stay consistent, (c) one bug you found and how, (d) what you would do next. Claude will
  question you on each.

## Resume line (draft, edit after Day 14, only keep what is true)

Built a single-threaded TCP key-value server in C++ speaking RESP over a non-blocking `poll()` event
loop, with a hand-written indexed min-heap for key expiry and a doubly linked list for LRU eviction;
verified with unit and randomised consistency tests, handling partial reads, pipelining and
malformed input.
