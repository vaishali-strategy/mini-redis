---
title: "Day 1: Sockets and your first TCP server"
subtitle: "Mini Redis project, beginner explainer"
---

Copy code from the `.md` file, not from this `.docx`, because Word can change quote characters and spacing.

# 1. What you are building today

A **server** is a program that waits for other programs (**clients**) to connect to it and then talks to them. Today's server does the simplest possible job: whatever a client sends, it sends straight back. This is called an **echo server**.

You will talk to it with `nc` (netcat), a tiny tool that opens a connection and lets you type.

By the end of the day you will know the six system calls that every TCP server in the world starts with, and you will understand why your first server can only look after **one client at a time**. That limitation is the reason Day 2 exists.

# 2. The idea in plain terms: a phone line

Think of a phone at a shop counter.

| Step | Phone world | Socket call |
|---|---|---|
| 1 | Buy a phone | `socket()` |
| 2 | Give it a number | `bind()` |
| 3 | Switch the ringer on | `listen()` |
| 4 | Pick up when it rings | `accept()` |
| 5 | Talk and listen | `read()` / `write()` |
| 6 | Hang up | `close()` |

One detail matters a lot: when `accept()` picks up a call, it gives you a **new line just for that caller**. The original phone (the one that rings) stays available for the next caller. So a server has two kinds of sockets:

- the **listening socket**: only waits for new callers;
- one **client socket per caller**: used to talk to that one caller.

# 3. Words you need

**File descriptor (fd).** A small integer the operating system gives you as a handle to something that can be read or written: a file, a pipe, or a socket. When the OS says "your socket is 3", the 3 is the file descriptor. Programs pass this number back to the OS in later calls ("read from 3").

**TCP.** The way most programs talk over a network. It gives you a reliable **stream of bytes** in order. It does **not** keep your messages separate. If a client sends "hello" and then "world", you might receive "helloworld" in one read, or "hel" and then "loworld" in two reads. TCP only promises the bytes arrive in order, not where one message ends. Remember this: it is why the RESP parser on Day 3 has to be able to stop in the middle of a command and continue later.

**Port.** A number from 0 to 65535 that says which program on a machine should get the connection. Your server will use 6380.

**Loopback address (127.0.0.1).** The address that means "this same machine". Using it keeps your server private to your Mac and avoids a macOS firewall pop-up.

**Byte order.** Networks send multi-byte numbers with the biggest byte first (big-endian). Your computer may store them the other way round. `htons` ("host to network, short") and `htonl` ("host to network, long") convert. You must call them for the port and the address.

# 4. The program

Create the folder `~/Projects/mini-redis/src/` and type this into `src/main.cpp`. Type it yourself; do not paste it. I compiled and tested this exact code: it builds with no warnings and echoes correctly for two connections one after the other.

```cpp
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>

int main() {
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
    if (listen(listen_fd, 16) < 0) {
        perror("listen");
        return 1;
    }
    std::printf("listening on 127.0.0.1:6380\n");

    while (true) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        char buf[1024];
        while (true) {
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) break;
            write(client_fd, buf, n);
        }
        close(client_fd);
    }
}
```

# 5. Walking through it

## The headers

- `<sys/socket.h>`: `socket`, `bind`, `listen`, `accept`.
- `<netinet/in.h>`: the `sockaddr_in` address record and constants like `INADDR_LOOPBACK`.
- `<arpa/inet.h>`: `htons` and `htonl`.
- `<unistd.h>`: `read`, `write`, `close`.
- `<cstdio>`: `printf` and `perror`.

## `socket(AF_INET, SOCK_STREAM, 0)`

Asks the OS for a new socket. `AF_INET` means IPv4. `SOCK_STREAM` means TCP (a byte stream). The last `0` means "choose the usual protocol for this". It returns a file descriptor, or `-1` if it fails.

Every OS call in this program can fail. The pattern is: check for a negative result, call `perror("name")` (which prints the name and the OS's reason, such as "Address already in use"), and stop or continue.

## `setsockopt(... SO_REUSEADDR ...)`

Without this, if you stop the server and start it again within about a minute, `bind` fails with "Address already in use", because the OS keeps the old connection around for a while. This option says "let me reuse the port straight away". You pass `&yes` (a variable holding 1) as the setting's value, and its size.

## `sockaddr_in addr{}`

An address record. The `{}` sets every field to zero first, which avoids garbage in the fields you do not fill in. Then you set three:

- `sin_family = AF_INET`: this is an IPv4 address.
- `sin_addr.s_addr = htonl(INADDR_LOOPBACK)`: listen on 127.0.0.1 only.
- `sin_port = htons(6380)`: port 6380, converted to network byte order.

## `bind(listen_fd, (sockaddr*)&addr, sizeof(addr))`

Attaches the socket to that address and port: "I am the program that answers on 127.0.0.1:6380". The cast `(sockaddr*)` is needed because these functions were designed for many address types and take a generic pointer. It will fail if another program already uses the port.

## `listen(listen_fd, 16)`

Switches the socket from "just exists" to "ready to receive callers". The `16` is the size of the waiting queue: up to 16 callers can be waiting while the server is busy.

## `accept(listen_fd, nullptr, nullptr)`

Waits (it **blocks**: the program stops here and does nothing else) until a client connects, then returns a **new** file descriptor, `client_fd`, for that one client. The two `nullptr`s mean "I do not need to know the client's address".

## `read(client_fd, buf, sizeof(buf))`

Waits for the client to send something, copies up to 1024 bytes into `buf`, and returns how many bytes it copied. Its return value has three meanings:

- **greater than 0**: that many bytes arrived;
- **0**: the client closed the connection (hung up);
- **-1**: an error.

That is why the loop does `if (n <= 0) break;`.

`ssize_t` is just a signed integer type for sizes, signed so it can hold -1.

## `write(client_fd, buf, n)`

Sends back exactly the `n` bytes you read. **Known limitation:** `write` is allowed to send fewer bytes than you asked for, and this code ignores that. It never matters for tiny echo messages, but it matters for real servers. Day 2 fixes it.

## `close(client_fd)`

Hangs up on this client and releases its file descriptor. If you forget, the descriptors pile up until the OS refuses to give you more (a **file descriptor leak**).

# 6. Build and run

From `~/Projects/mini-redis`:

```bash
clang++ -std=c++23 -Wall -Wextra -O2 src/main.cpp -o server
./server
```

You should see `listening on 127.0.0.1:6380`.

In a **second terminal window**:

```bash
nc 127.0.0.1 6380
```

Type `hello` and press Enter. The server sends `hello` back. Press Ctrl-C in `nc` to disconnect; the server keeps running and waits for the next caller. Connect again to confirm.

# 7. Day 1's key observation

Keep the first `nc` connected. Open a **third** terminal and run `nc 127.0.0.1 6380` again. Type something in the third window.

**Nothing comes back** until you close the first one.

Why? The server is inside the inner `read` loop for client one, waiting for client one to speak. It cannot go back to `accept` to look at client two. The second client is sitting in the waiting queue from `listen(..., 16)`. One blocked call is holding up everyone.

This is the whole motivation for Day 2: an **event loop** with `poll()` that waits on *many* sockets at once and only touches the ones that are ready.

# 8. Break it on purpose

Do these and write down what you saw. Each one teaches a real failure.

1. **Port already in use.** Leave the server running. In another terminal, start a second copy of `./server`. What does `perror` print, and from which call?
2. **Remove `SO_REUSEADDR`.** Delete the `setsockopt` line, rebuild, connect with `nc`, close `nc`, stop the server with Ctrl-C, and start it again straight away. What happens? Put the line back.
3. **Forget the byte-order conversion.** Change `htons(6380)` to just `6380`. Rebuild, run, and try `nc 127.0.0.1 6380`. What port is it really listening on, and why?
4. **Delete the `close`.** Comment out `close(client_fd)`. Connect and disconnect `nc` a few times. What does the server do now? (Hint: think about what the descriptor numbers do.)

# 9. Your job today

1. Type the program yourself, compile it, run it, and test it with `nc`.
2. Without looking at this document, write **one sentence per call** in your own words: `socket`, `bind`, `listen`, `accept`, `read`, `close`.
3. Do the four break-it experiments and write down what you saw.
4. Answer the defence questions below.
5. Send `src/main.cpp` and your sentences to Claude to check.

# 10. Defence questions

Answer these without notes before Day 2.

1. What is a file descriptor?
2. Why is TCP described as a stream of bytes with no message boundaries? Give an example of what that does to a reader.
3. What does `accept` return, and how is that socket different from the listening socket?
4. What does `read` returning 0 mean? What would go wrong if the code did not check for it?
5. Why does the second `nc` get no reply while the first is connected?

# 11. Quick glossary

| Term | Meaning |
|---|---|
| socket | an OS handle for one end of a network connection |
| blocking | the call waits and the program does nothing else until it finishes |
| listening socket | the one that waits for new callers |
| client socket | the one returned by `accept`, used to talk to one caller |
| echo server | a server that sends back what it received |
| loopback | the address 127.0.0.1, meaning this same machine |
| port | a number that picks which program receives a connection |
| `perror` | prints your message plus the OS's reason for the last failed call |
