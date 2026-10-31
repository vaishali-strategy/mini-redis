# Mini Redis

A single-threaded in-memory key-value server in C++. One `poll()` event loop serves many
clients over TCP using the RESP wire protocol (the one `redis-cli` speaks), with key expiry
driven by a hand-written indexed min-heap and least-recently-used eviction driven by a
hand-written doubly linked list.

No third-party code: the standard library and POSIX sockets only.

## Build and run

```bash
make                       # builds ./server  (clang++, C++23)
./server                   # listens on 127.0.0.1:6380
./server --port 7000 --max-keys 1000
```

```bash
redis-cli -p 6380 set greeting hello
redis-cli -p 6380 expire greeting 10
redis-cli -p 6380 ttl greeting
```

## Commands

| Command | Reply |
|---|---|
| `PING [msg]` | `PONG`, or `msg` |
| `SET key value` | `OK`. Overwriting a key clears its TTL |
| `GET key` | the value, or nil |
| `DEL key [key ...]` | how many of those keys existed |
| `EXPIRE key seconds` | `1` if set, `0` if no such key. `seconds <= 0` deletes the key |
| `TTL key` | seconds left; `-1` = no TTL; `-2` = no such key |
| `DBSIZE` | number of keys |

Command names are case-insensitive. Values are binary-safe.

## How it works

```
 clients ──TCP──> poll() loop ──> RESP parser ──> command ──> Store
                  (server.cpp)    (resp.cpp)    (commands.cpp)  (store.cpp)
                                                                  │
                                         unordered_map  ◄─────────┤ find a key: O(1)
                                         ExpiryHeap     ◄─────────┤ next key to expire: O(1), fix: O(log n)
                                         LruList        ◄─────────┘ next key to evict: O(1), touch: O(1)
```

**Event loop.** One thread, no locks. Every round, `poll()` sleeps until a socket is ready *or*
until the earliest key is due to expire (the heap's top sets the timeout). Sockets are
non-blocking. Each client has an input buffer (bytes that are not yet a whole command) and
an output buffer (replies not yet sent), so a short read or a short write never loses data.

**Parser.** `parse_command` looks at a buffer and answers *Ok* (one command, and how many bytes it
used), *NeedMore* (valid so far, incomplete) or *Error*. Because TCP has no message boundaries,
a command can arrive in pieces and several can arrive at once; the server loops until
*NeedMore*, which is what makes pipelining work. Lengths are checked before any body is waited
for, so a client cannot make the server wait for or allocate gigabytes.

**One key, three structures.** Each key's `Entry` is in the hash map, in the LRU list, and
(if it has a TTL) in the expiry heap. The heap and list hold pointers to the entry, and the
entry remembers its own heap position and list neighbours, so any of them can be unlinked in
O(log n) / O(1) without searching. `Store::remove()` is the single place that unlinks a key
from all three; DEL, expiry and eviction all go through it.

**Expiry** is both lazy (an overdue key is deleted when anyone looks it up) and active (the
loop deletes everything due, even keys nobody asks for).

## Limits

- Listens on loopback only. Single-threaded. In memory only (no persistence).
- One argument is at most 1 MiB; a command has at most 8 arguments; a client whose unsent
  replies exceed 64 MiB is dropped.
- Invalid input gets `-ERR protocol error` and the connection is closed.
- Inline (plain text) commands are not supported; clients must speak RESP.
- Only the commands in the table above.

## Tests

```bash
make test        # unit tests + protocol tests
make asan        # the same, built with AddressSanitizer and UBSan
```

| Test | What it covers |
|---|---|
| `tests/test_resp.cpp` | parser: byte-at-a-time, split CRLF, pipelined input, every kind of malformed input, a fuzz loop |
| `tests/test_heap.cpp` | heap: ordering, ties, erase from root/middle/end, update earlier/later, randomised against `std::multiset` |
| `tests/test_lru.cpp` | list: head/middle/tail edge cases, one-element list, randomised against `std::list` |
| `tests/test_store.cpp` | TTL rules, lazy and active expiry, eviction order, and a differential test: 180,000 random operations compared against a separate simple reference model, with the three structures cross-checked after every few steps |
| `tests/test_protocol.py` | a real server over real sockets: partial reads at every split point, 2000 pipelined pairs, 12 kinds of bad input, size limits, 120 clients that vanish mid-command, half-close with queued replies, expiry with nobody asking, eviction, 40 concurrent clients, a slow reader |

The tests were also checked by breaking the code on purpose (removing the heap re-balancing,
the LRU touch on `GET`, the active expiry sweep, and so on, 15 mutations in all): each
mutation made at least one test fail.

Sanitizer result: _not yet recorded. Run `make asan` yourself and write down what it prints.
Note that AddressSanitizer on macOS finds memory errors but does not do leak checking._

## Benchmark

`make bench` (needs `redis-benchmark`). Setup: Apple M2, 8 cores, macOS (Darwin 25.6.0), client and
server on the same machine over loopback, `-O2`, `redis-benchmark -t set,get -n 100000 -r 10000`,
3 runs per row, run on 30 Oct 2026. Numbers are requests per second; they move with whatever else
the machine is doing, so treat them as a range, not a constant.

| Clients | Pipeline | SET | GET |
|---|---|---|---|
| 1 | 1 | 47,700 - 48,900 | 48,400 - 49,000 |
| 10 | 1 | 172,700 - 190,500 | 174,200 - 185,900 |
| 50 | 1 | 181,800 - 182,500 | 183,100 - 184,200 |
| 50 | 16 | 2,325,600 - 2,381,000 | 2,173,900 - 2,381,000 |

What the numbers show: throughput stops growing between 10 and 50 clients (one thread is the
limit), and pipelining 16 commands per round trip is about 13 times faster than one at a
time. That fits a per-request cost dominated by system calls (one `read` and one `write` each),
which pipelining amortises. This has not been profiled.
