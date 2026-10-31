#pragma once
// The network layer: one thread, one poll() loop, many clients.

#include <cstddef>
#include <cstdint>

// Listens on 127.0.0.1:port and serves forever. Returns non-zero if it cannot start.
// max_keys == 0 means no limit on the number of keys.
int run_server(uint16_t port, size_t max_keys);
