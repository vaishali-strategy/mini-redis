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

using namespace std;
struct Client
{
    int fd;
    string in;
    string out;
};
static void set_nonblocking(int fd)
{
    int flags=fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
int main() {
    signal(SIGPIPE, SIG_IGN);

    int listen_fd=socket(AF_INET, SOCK_STREAM,0);
    if (listen_fd < 0)
    {
        perror("socket");
        return 1;
    }
    int yes=1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    addr.sin_port=htons(6380);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 16)<0)
    {
        perror("listen");
        return 1;
    }
    set_nonblocking(listen_fd);
    printf("listening on 127.0.0.1:6380\n");
    fflush(stdout);

    unordered_map<int, Client> clients;

    while(true)
    {
        vector<pollfd> fds;
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