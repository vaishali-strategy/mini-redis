#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "server.hpp"

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--max-keys N]\n"
                 "  --port N      TCP port to listen on (default 6380)\n"
                 "  --max-keys N  evict least recently used keys beyond N keys (default: no limit)\n",
                 prog);
}

bool parse_number(const char* s, unsigned long long max, unsigned long long& out) {
    if (*s == '\0' || *s == '-') return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (errno != 0 || *end != '\0' || v > max) return false;
    out = v;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 6380;
    size_t max_keys = 0;

    for (int i = 1; i < argc; ++i) {
        unsigned long long v = 0;
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc &&
            parse_number(argv[i + 1], 65535, v) && v != 0) {
            port = static_cast<uint16_t>(v);
            ++i;
        } else if (std::strcmp(argv[i], "--max-keys") == 0 && i + 1 < argc &&
                   parse_number(argv[i + 1], 1ULL << 40, v)) {
            max_keys = static_cast<size_t>(v);
            ++i;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    return run_server(port, max_keys);
}
