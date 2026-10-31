#include "server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "commands.hpp"
#include "resp.hpp"
#include "store.hpp"

namespace {

constexpr size_t kReadChunk = 64 * 1024;
constexpr size_t kMaxOutBuffer = 64 * 1024 * 1024;  // a client that never reads its replies is dropped

struct Client {
    int fd;
    std::string in;           // bytes received that do not yet form a complete command
    std::string out;          // replies waiting to be sent
    bool closing = false;     // stop reading; close as soon as `out` has been sent
};

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Run every complete command sitting in c.in. Returns false if the client must be dropped now.
bool process_input(Client& c, Store& store) {
    size_t off = 0;
    while (off < c.in.size()) {
        ParseResult r = parse_command(c.in.data() + off, c.in.size() - off);
        if (r.status == ParseStatus::NeedMore) break;
        if (r.status == ParseStatus::Error) {
            // Replies to earlier, valid commands in this batch are already in c.out.
            append_error(c.out, "ERR protocol error");
            c.closing = true;
            c.in.clear();
            return true;
        }
        execute_command(store, r.args, c.out);
        off += r.consumed;
        if (c.out.size() > kMaxOutBuffer) return false;
    }
    c.in.erase(0, off);  // keep only the unfinished tail
    return true;
}

// Read once from a ready client. Returns false if the client must be dropped now.
bool read_from(Client& c, Store& store) {
    char buf[kReadChunk];
    ssize_t n = read(c.fd, buf, sizeof(buf));
    if (n > 0) {
        c.in.append(buf, static_cast<size_t>(n));
        return process_input(c, store);
    }
    if (n == 0) {  // the client closed its side; still deliver replies already queued
        c.closing = true;
        c.in.clear();
        return true;
    }
    return errno == EAGAIN || errno == EINTR;
}

// Send as much of c.out as the OS will take. Returns false on a real error.
bool write_to(Client& c) {
    if (c.out.empty()) return true;
    ssize_t n = write(c.fd, c.out.data(), c.out.size());
    if (n > 0) {
        c.out.erase(0, static_cast<size_t>(n));  // keep whatever was not sent yet
        return true;
    }
    return n < 0 && (errno == EAGAIN || errno == EINTR);
}

}  // namespace

int run_server(uint16_t port, size_t max_keys) {
    // Writing to a connection the client already closed would otherwise kill the process.
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        return 1;
    }
    set_nonblocking(listen_fd);
    std::printf("listening on 127.0.0.1:%u (max keys: %s)\n", static_cast<unsigned>(port),
                max_keys == 0 ? "unlimited" : std::to_string(max_keys).c_str());
    std::fflush(stdout);

    Store store(max_keys);
    std::unordered_map<int, Client> clients;
    std::vector<pollfd> fds;

    while (true) {
        // 1. What do we want to hear about?
        fds.clear();
        fds.push_back({listen_fd, POLLIN, 0});
        for (auto& [fd, c] : clients) {
            short events = 0;
            if (!c.closing) events |= POLLIN;
            if (!c.out.empty()) events |= POLLOUT;
            fds.push_back({fd, events, 0});
        }

        // 2. Sleep until a socket is ready, or until the earliest key is due to expire.
        int timeout_ms = -1;
        if (auto next = store.next_expiry()) {
            int64_t wait = *next - store.now();
            timeout_ms = wait <= 0 ? 0 : static_cast<int>(std::min<int64_t>(wait, INT_MAX));
        }
        if (poll(fds.data(), fds.size(), timeout_ms) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return 1;
        }

        // 3. Active expiry: delete keys whose time has come, whether or not anyone asked.
        store.expire_due();

        // 4. New connections.
        if (fds[0].revents & POLLIN) {
            while (true) {
                int cfd = accept(listen_fd, nullptr, nullptr);
                if (cfd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR &&
                        errno != ECONNABORTED)
                        perror("accept");
                    break;
                }
                set_nonblocking(cfd);
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
                clients.emplace(cfd, Client{cfd, {}, {}, false});
            }
        }

        // 5. Existing clients.
        for (size_t i = 1; i < fds.size(); ++i) {
            auto it = clients.find(fds[i].fd);
            if (it == clients.end()) continue;
            Client& c = it->second;
            short re = fds[i].revents;
            bool alive = true;

            if (!c.closing && (re & (POLLIN | POLLHUP | POLLERR))) alive = read_from(c, store);
            if (alive) alive = write_to(c);
            if (alive && c.closing && c.out.empty()) alive = false;  // everything sent: close now
            if (re & POLLNVAL) alive = false;

            if (!alive) {
                close(c.fd);
                clients.erase(it);
            }
        }
    }
}
