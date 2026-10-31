---
title: "Day 2: Many clients with poll()"
subtitle: "Mini Redis project, beginner explainer"
---

Copy code from the `.md` file, not from this `.docx`.

# 1. The problem you saw on Day 1

Your Day 1 server looks after one client at a time. While it is inside `read` for client one, it cannot go back to `accept` for client two. The `read` call **blocks**: the whole program stops and waits.

A real server must look after many clients at once. There are two classic ways to do that:

1. **One thread per client.** Simple to think about, but costly with many clients, and you need locks as soon as clients share data.
2. **One thread, one event loop.** Ask the OS "which of my sockets are ready?", then handle only those. Nothing ever waits on one slow client.

Redis uses the second way, and so will you. It also means no locks, because only one thing ever runs at a time.

# 2. The idea: a receptionist who checks all phones

Imagine one receptionist and ten phones. A bad receptionist stares at phone 1 until it rings. A good one has a board that lights up the phones that are ringing, and handles only those.

`poll()` is that board. You give it a list of file descriptors and say what you care about for each one. It sleeps until at least one is ready, then tells you which.

# 3. Words you need

**Non-blocking socket.** A socket set so that `read`, `write` and `accept` never wait. If there is nothing to do, they return -1 immediately and set `errno` to `EAGAIN` (meaning "not now, try later"). That is not an error, it is a normal answer.

**Event loop.** A loop that does: ask `poll()` what is ready, handle those sockets, repeat forever.

**`pollfd`.** A small record with three fields: `fd` (which socket), `events` (what you want to hear about), `revents` (what actually happened, filled in by `poll`).

**`POLLIN`.** "There is something to read" (for the listening socket: "a caller is waiting").

**`POLLOUT`.** "You can write now without it being refused."

**Partial write.** `write` is allowed to send only some of your bytes. If the other side is slow to read, the OS buffer fills up and `write` takes only what fits. You must keep the rest and try again later.

**`SIGPIPE`.** A signal the OS sends your program if you write to a connection the other side has already closed. By default it kills the whole program. A server must ignore it.

# 4. Design of this version

Each client gets a record:

```
struct Client { fd; in; out; }
```

- `in` holds bytes received but not used yet. Today it stays unused; on Day 3 the RESP parser will read from it.
- `out` holds bytes waiting to be sent. Instead of writing straight away and hoping, you put the reply into `out`, and send as much as the OS will take. Whatever is left stays in `out` until the next round.

All clients live in a table keyed by file descriptor: `std::unordered_map<int, Client>`.

Each loop round does four things:

1. **Build the list** for `poll`: the listening socket (`POLLIN`) plus every client (`POLLIN`, and also `POLLOUT` if that client has bytes waiting in `out`).
2. **Call `poll`** and sleep until something is ready.
3. **Accept** every waiting caller if the listening socket is ready.
4. **For each ready client:** read what arrived, queue the echo into `out`, write as much of `out` as possible, and remove the client if it hung up or errored.

# 5. The program

Replace `src/main.cpp`. Type it; do not paste it. I compiled this exact code with no warnings and tested it.

```cpp
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

struct Client {
    int fd;
    std::string in;   // bytes received, not yet used (the parser will eat these on Day 3)
    std::string out;  // bytes waiting to be sent
};

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    // A write to a socket the client already closed would otherwise kill the whole server.
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
    addr.sin_port = htons(6380);
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        return 1;
    }
    set_nonblocking(listen_fd);
    std::printf("listening on 127.0.0.1:6380\n");
    std::fflush(stdout);

    std::unordered_map<int, Client> clients;

    while (true) {
        // 1. Tell poll() what we are interested in.
        std::vector<pollfd> fds;
        fds.push_back({listen_fd, POLLIN, 0});
        for (auto& [fd, c] : clients) {
            short events = POLLIN;
            if (!c.out.empty()) events |= POLLOUT;
            fds.push_back({fd, events, 0});
        }

        // 2. Sleep until something is ready.
        if (poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return 1;
        }

        // 3. New callers waiting on the listening socket?
        if (fds[0].revents & POLLIN) {
            while (true) {
                int cfd = accept(listen_fd, nullptr, nullptr);
                if (cfd < 0) break;  // EAGAIN: nobody else is waiting right now
                set_nonblocking(cfd);
                clients[cfd] = Client{cfd, "", ""};
            }
        }

        // 4. Serve every client that poll() marked ready.
        for (size_t i = 1; i < fds.size(); ++i) {
            auto it = clients.find(fds[i].fd);
            if (it == clients.end()) continue;
            Client& c = it->second;
            short re = fds[i].revents;
            bool dead = false;

            if (re & (POLLIN | POLLHUP | POLLERR)) {
                char buf[4096];
                ssize_t n = read(c.fd, buf, sizeof(buf));
                if (n > 0) {
                    c.out.append(buf, n);  // echo: queue the bytes to send back
                } else if (n == 0) {
                    dead = true;           // client hung up
                } else if (errno != EAGAIN && errno != EINTR) {
                    dead = true;           // real error
                }
            }

            if (!dead && !c.out.empty()) {
                ssize_t n = write(c.fd, c.out.data(), c.out.size());
                if (n > 0) {
                    c.out.erase(0, n);     // keep the part that was not sent yet
                } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                    dead = true;
                }
            }

            if (dead) {
                close(c.fd);
                clients.erase(it);
            }
        }
    }
}
```

# 6. Walking through the new parts

**`#include <poll.h>`, `<fcntl.h>`, `<csignal>`, `<cerrno>`.** `poll.h` gives `poll` and `pollfd`. `fcntl.h` gives `fcntl` and `O_NONBLOCK`. `csignal` gives `signal`. `cerrno` gives `errno`, `EAGAIN` and `EINTR`.

**`signal(SIGPIPE, SIG_IGN)`.** "If the OS tries to send me SIGPIPE, ignore it." After this, a write to a dead connection simply returns -1 instead of killing the server.

**`set_nonblocking`.** `fcntl(fd, F_GETFL)` reads the socket's current flags. `F_SETFL` sets them again with `O_NONBLOCK` added. The `|` combines flags without losing the old ones. Both the listening socket and every client socket get this.

**`listen(listen_fd, 128)`.** The waiting queue is now 128. The number is only a hint to the OS.

**`std::unordered_map<int, Client> clients`.** Look up a client by its file descriptor.

**Building `fds`.** `fds[0]` is always the listening socket. Clients follow. `{listen_fd, POLLIN, 0}` fills `fd`, `events`, `revents`.

**`if (!c.out.empty()) events |= POLLOUT`.** You only ask to hear about writability when you actually have something to send. If you always asked, `poll` would return immediately every round (a socket is almost always writable) and the loop would spin at 100% CPU.

**`poll(fds.data(), fds.size(), -1)`.** Pass the array, its length, and a timeout in milliseconds. `-1` means wait forever. On Day 8 you will pass a real timeout, based on the next key due to expire. If `poll` is interrupted by a signal it returns -1 with `errno == EINTR`, which is not a failure: just go round again.

**The accept loop.** One `POLLIN` on the listening socket can mean several callers are waiting, so you call `accept` repeatedly. Because the socket is non-blocking, `accept` returns -1 with `EAGAIN` when the queue is empty, and the loop stops.

**`Client& c = it->second`.** A reference to the client's record, so changes affect the one stored in the map.

**Reading.** `re & (POLLIN | POLLHUP | POLLERR)` means "readable, or the connection closed, or an error". In the last two cases `read` will return 0 or -1, which tells you what happened. Results:

- `n > 0`: data arrived; append it to `out` to echo it.
- `n == 0`: the client hung up; mark it dead.
- `n < 0` with `EAGAIN`: nothing to read right now; ignore.
- `n < 0` with another error: mark it dead.

**Writing.** Try to send everything in `out`. `write` returns how many bytes the OS took. `out.erase(0, n)` removes exactly those from the front, so what is left is what still needs sending. That is how partial writes are handled. If it returns -1 with `EAGAIN`, the OS buffer is full: leave `out` alone and try next round.

**Cleaning up.** `close` the descriptor and erase the client from the map, so nothing leaks. Erasing from `clients` while looping over the `fds` vector is safe, because the vector is a separate copy.

# 7. Known limitations (on purpose, fixed later)

- `out` can grow without limit if a client sends a lot but never reads the replies. Day 5 adds size limits.
- The echo is a placeholder. Day 4 replaces it with real commands.

# 8. Build and test

```bash
make
./server
```

**Stop your old Day 1 server first** (Ctrl-C in its terminal), or `bind` will fail with "Address already in use".

Then:

1. Open three terminals and run `nc 127.0.0.1 6380` in each. Type in all three. **Each one now echoes** while the others are connected. That was impossible on Day 1.
2. Press Ctrl-C in one `nc`. The other two keep working.
3. Send something large with `head -c 3000000 /dev/urandom | nc 127.0.0.1 6380 | wc -c` and check it prints 3000000 (this proves partial writes are handled; stop with Ctrl-C if `nc` waits).

# 9. Break it on purpose

1. **Remove `signal(SIGPIPE, SIG_IGN)`.** Rebuild. Connect with `nc` and send a large amount of data, then kill `nc` mid-transfer. Does the server survive? (It may not show up every time. Say what you saw.)
2. **Always ask for `POLLOUT`.** Change `events |= POLLOUT` so it happens even when `out` is empty. Watch the server's CPU in Activity Monitor with one idle client connected. Put it back.
3. **Make `listen_fd` blocking again** (comment out `set_nonblocking(listen_fd)`). Connect one client. What happens in the accept loop, and why is it a problem?
4. **Remove the `close`/`erase` on dead clients.** Connect and disconnect `nc` ten times. What does the server do, and why?
5. **Change `out.erase(0, n)` to `out.clear()`.** Echo 3 MB. What goes wrong?

# 10. Your job today

1. Type the program, compile it, and test it with three `nc` sessions.
2. Write one sentence each, in your own words: non-blocking, `EAGAIN`, `POLLIN`, `POLLOUT`, partial write.
3. Do the five break-it experiments and note what you saw.
4. Answer the defence questions.
5. Send `src/main.cpp` and your answers.

# 11. Defence questions

Answer these without notes.

1. Why one thread and many clients, instead of one thread per client?
2. What does `poll()` tell you, and what does it not tell you?
3. Why must the listening socket be non-blocking too?
4. What breaks if `write` sends only part of the reply and the code ignores it?
5. Why do we only ask for `POLLOUT` when `out` is not empty?
6. What does `read` returning 0 mean, and what happens to the client record then?
