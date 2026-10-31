#!/usr/bin/env python3
"""Protocol tests: start the real server binary and talk to it over a real socket.

usage: python3 tests/test_protocol.py ./server
Standard library only.
"""
import os
import socket
import subprocess
import sys
import threading
import time

SERVER = sys.argv[1] if len(sys.argv) > 1 else "./server"
failures = 0
checks = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {msg}")


# ---------------------------------------------------------------- helpers
def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server:
    def __init__(self, *extra):
        self.port = free_port()
        self.proc = subprocess.Popen(
            [SERVER, "--port", str(self.port), *extra],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=5)

    def conn(self):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return Conn(s)


def enc(*parts):
    out = b"*%d\r\n" % len(parts)
    for p in parts:
        p = p if isinstance(p, bytes) else str(p).encode()
        out += b"$%d\r\n%s\r\n" % (len(p), p)
    return out


class Conn:
    def __init__(self, sock):
        self.s = sock
        self.buf = b""

    def _fill(self):
        d = self.s.recv(65536)
        if not d:
            raise EOFError
        self.buf += d

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def reply(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t == b"+":
            return rest.decode()
        if t == b"-":
            return Exception(rest.decode())
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            while len(self.buf) < n + 2:
                self._fill()
            data, self.buf = self.buf[:n], self.buf[n + 2:]
            return data
        raise ValueError(line)

    def cmd(self, *parts):
        self.s.sendall(enc(*parts))
        return self.reply()

    def closed_by_server(self, wait=2.0):
        """True if the server closes this connection (after sending anything it still owes)."""
        self.s.settimeout(wait)
        try:
            while True:
                if not self.s.recv(65536):
                    return True
        except (socket.timeout, TimeoutError):
            return False
        except ConnectionResetError:
            return True

    def close(self):
        self.s.close()

    def rst_close(self):
        import struct
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.s.close()


def is_err(r, prefix=""):
    return isinstance(r, Exception) and str(r).startswith(prefix)


# ---------------------------------------------------------------- tests
def test_basic(srv):
    c = srv.conn()
    check(c.cmd("PING") == "PONG", "PING")
    check(c.cmd("SET", "a", "1") == "OK", "SET")
    check(c.cmd("GET", "a") == b"1", "GET")
    check(c.cmd("GET", "nope") is None, "GET missing is nil")
    check(c.cmd("DEL", "a", "zz") == 1, "DEL counts existing keys")
    check(c.cmd("GET", "a") is None, "GET after DEL")
    check(is_err(c.cmd("NOPE"), "ERR unknown command"), "unknown command")
    check(is_err(c.cmd("GET"), "ERR wrong number"), "wrong arity")
    check(c.cmd("set", "lower", "case") == "OK" and c.cmd("get", "lower") == b"case", "case-insensitive")
    check(c.cmd("SET", "bin", b"a\r\nb\x00c") == "OK" and c.cmd("GET", "bin") == b"a\r\nb\x00c",
          "binary-safe value")
    check(c.cmd("SET", "empty", "") == "OK" and c.cmd("GET", "empty") == b"", "empty value")
    c.close()


def test_partial_reads(srv):
    c = srv.conn()
    wire = enc("SET", "partial", "value-sent-one-byte-at-a-time")
    for i in range(len(wire)):
        c.s.sendall(wire[i:i + 1])
        time.sleep(0.002)
    check(c.reply() == "OK", "byte-at-a-time SET")
    check(c.cmd("GET", "partial") == b"value-sent-one-byte-at-a-time", "value arrived intact")

    # split exactly in the middle of the \r\n and in the middle of a value
    wire = enc("GET", "partial")
    for cut in range(1, len(wire)):
        c.s.sendall(wire[:cut])
        time.sleep(0.003)
        c.s.sendall(wire[cut:])
        check(c.reply() == b"value-sent-one-byte-at-a-time", f"command split at byte {cut}")
    c.close()


def test_pipelining(srv):
    c = srv.conn()
    n = 2000
    batch = b"".join(enc("SET", f"p{i}", f"v{i}") + enc("GET", f"p{i}") for i in range(n))
    c.s.sendall(batch)
    ok = True
    for i in range(n):
        if c.reply() != "OK" or c.reply() != f"v{i}".encode():
            ok = False
            break
    check(ok, f"{n} pipelined SET+GET pairs answered in order")

    # a pipelined batch that ends in the middle of a command
    wire = enc("SET", "tail", "x") + enc("GET", "tail")
    c.s.sendall(wire[:-5])
    check(c.reply() == "OK", "first command of a cut batch answered")
    c.s.sendall(wire[-5:])
    check(c.reply() == b"x", "second command completed after the rest arrived")
    c.close()


def test_malformed(srv):
    bad = {
        "not RESP": b"HELLO\r\n",
        "inline command": b"PING\r\n",
        "zero-length array": b"*0\r\n",
        "negative array": b"*-1\r\n",
        "too many args": b"*9\r\n",
        "non-numeric count": b"*abc\r\n",
        "negative bulk length": b"*1\r\n$-1\r\n",
        "bulk missing CRLF": b"*1\r\n$3\r\nabcXX",
        "wrong element type": b"*1\r\n+OK\r\n",
        "huge declared length": b"*1\r\n$999999999999\r\n",
        "just over the 1 MiB limit": b"*1\r\n$%d\r\n" % ((1 << 20) + 1),
        "endless header": b"*" + b"1" * 5000,
    }
    for name, payload in bad.items():
        c = srv.conn()
        c.s.sendall(payload)
        try:
            r = c.reply()
            check(is_err(r, "ERR protocol error"), f"{name}: error reply (got {r!r})")
        except EOFError:
            check(False, f"{name}: connection closed without an error reply")
        check(c.closed_by_server(), f"{name}: connection closed")
        c.close()
        other = srv.conn()
        check(other.cmd("PING") == "PONG", f"{name}: server still serves other clients")
        other.close()

    # valid commands before the bad one still get their answers
    c = srv.conn()
    c.s.sendall(enc("SET", "before", "1") + b"garbage\r\n")
    check(c.reply() == "OK", "valid command before garbage is answered")
    check(is_err(c.reply(), "ERR protocol error"), "then the error")
    check(c.closed_by_server(), "then close")
    c.close()
    check(srv.alive(), "server survived all malformed input")


def test_sizes(srv):
    c = srv.conn()
    big = os.urandom(1 << 20)  # exactly the limit: allowed
    check(c.cmd("SET", "big", big) == "OK", "1 MiB value accepted")
    check(c.cmd("GET", "big") == big, "1 MiB value read back identical")
    c.close()
    c = srv.conn()
    c.s.sendall(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$%d\r\n" % ((1 << 20) + 1))
    check(is_err(c.reply(), "ERR protocol error"), "1 MiB + 1 rejected from the length alone")
    c.close()


def test_disconnects(srv):
    wire = enc("SET", "half", "x" * 5000)
    for i in range(60):
        c = srv.conn()
        c.s.sendall(wire[: 10 + (i * 37) % (len(wire) - 20)])
        if i % 2:
            c.rst_close()
        else:
            c.close()
    # clients that send a command and vanish before reading the answer
    for _ in range(60):
        c = srv.conn()
        c.s.sendall(enc("GET", "big") * 5)
        c.rst_close()
    time.sleep(0.2)
    check(srv.alive(), "server alive after clients vanished mid-command")
    c = srv.conn()
    check(c.cmd("PING") == "PONG", "server still serves after the disconnect storm")
    check(c.cmd("GET", "half") is None, "half-sent command was never executed")
    c.close()

    # half-close: client sends, shuts down its write side, must still receive the reply
    c = srv.conn()
    c.s.sendall(enc("PING"))
    c.s.shutdown(socket.SHUT_WR)
    check(c.reply() == "PONG", "reply delivered after client half-closed")
    check(c.closed_by_server(), "then server closes")
    c.close()


def test_expiry(srv):
    c = srv.conn()
    c.cmd("SET", "t", "v")
    check(c.cmd("TTL", "t") == -1, "no TTL = -1")
    check(c.cmd("TTL", "missing") == -2, "missing = -2")
    check(c.cmd("EXPIRE", "t", 1) == 1, "EXPIRE returns 1")
    check(c.cmd("EXPIRE", "missing", 1) == 0, "EXPIRE on missing key returns 0")
    check(c.cmd("TTL", "t") in (0, 1), "TTL right after EXPIRE")
    check(is_err(c.cmd("EXPIRE", "t", "abc"), "ERR value is not an integer"), "non-integer seconds")

    # Active expiry: after the TTL, DBSIZE (which does not touch the key) shows it is gone.
    c.cmd("SET", "other", "stays")
    before = c.cmd("DBSIZE")
    time.sleep(1.4)
    after = c.cmd("DBSIZE")
    check(after == before - 1, f"key removed with nobody asking for it ({before} -> {after})")
    check(c.cmd("GET", "t") is None and c.cmd("GET", "other") == b"stays", "right key removed")

    # SET clears a TTL
    c.cmd("SET", "s", "1")
    c.cmd("EXPIRE", "s", 1)
    c.cmd("SET", "s", "2")
    check(c.cmd("TTL", "s") == -1, "SET cleared the TTL")
    time.sleep(1.3)
    check(c.cmd("GET", "s") == b"2", "key with cleared TTL survived")

    # EXPIRE twice: the later one wins
    c.cmd("SET", "m", "1")
    c.cmd("EXPIRE", "m", 100)
    c.cmd("EXPIRE", "m", 1)
    time.sleep(1.3)
    check(c.cmd("GET", "m") is None, "shortened TTL took effect")

    # DEL a key with a TTL, then let its old deadline pass: the server must not trip over it
    c.cmd("SET", "d", "1")
    c.cmd("EXPIRE", "d", 1)
    check(c.cmd("DEL", "d") == 1, "DEL key with TTL")
    time.sleep(1.3)
    check(c.cmd("PING") == "PONG" and srv.alive(), "server fine after a deleted key's deadline")

    # many keys with different TTLs expiring in order
    baseline = c.cmd("DBSIZE")
    for i in range(50):
        c.cmd("SET", f"e{i}", "v")
        c.cmd("EXPIRE", f"e{i}", 1 + i % 2)
    check(c.cmd("DBSIZE") == baseline + 50, "50 timed keys added")
    time.sleep(2.4)
    after = c.cmd("DBSIZE")
    check(after == baseline, f"all 50 timed keys gone with nobody asking ({baseline} -> {after})")
    c.close()


def test_eviction():
    srv = Server("--max-keys", "3")
    c = srv.conn()
    for k in "abc":
        c.cmd("SET", k, k)
    c.cmd("GET", "a")          # a is recent; b is the least recently used
    c.cmd("SET", "d", "d")     # over the limit: b goes
    check(c.cmd("DBSIZE") == 3, "never more than 3 keys")
    check(c.cmd("GET", "b") is None, "least recently used key evicted")
    check(c.cmd("GET", "a") == b"a" and c.cmd("GET", "c") == b"c" and c.cmd("GET", "d") == b"d",
          "others kept")

    # evicted keys with TTLs must leave no trace in the expiry heap
    c.cmd("SET", "ttl1", "x")
    c.cmd("EXPIRE", "ttl1", 1)
    c.cmd("SET", "n1", "x")
    c.cmd("SET", "n2", "x")
    c.cmd("SET", "n3", "x")
    check(c.cmd("TTL", "ttl1") == -2, "evicted key is gone")
    time.sleep(1.4)
    check(c.cmd("PING") == "PONG" and srv.alive(), "server fine when the evicted key's deadline passes")

    for i in range(2000):      # churn
        c.cmd("SET", f"churn{i}", i)
        if i % 3 == 0:
            c.cmd("EXPIRE", f"churn{i}", 1)
    check(c.cmd("DBSIZE") == 3, "still 3 keys after heavy churn")
    check(srv.alive(), "alive after churn")
    c.close()
    srv.stop()


def test_many_clients(srv):
    errors = []

    def worker(n):
        try:
            c = srv.conn()
            for i in range(100):
                k = f"w{n}-{i}"
                if c.cmd("SET", k, k) != "OK" or c.cmd("GET", k) != k.encode():
                    errors.append(k)
            c.close()
        except Exception as e:  # noqa
            errors.append(repr(e))

    threads = [threading.Thread(target=worker, args=(n,)) for n in range(40)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    check(not errors, f"40 concurrent clients x 100 SET/GET ({errors[:3]})")


def test_slow_reader(srv):
    """A client that pipelines many large replies and reads them slowly: replies must not be lost."""
    c = srv.conn()
    val = os.urandom(200_000)
    c.cmd("SET", "slow", val)
    c.s.sendall(enc("GET", "slow") * 40)  # 8 MB of replies queued
    time.sleep(0.5)
    ok = True
    for _ in range(40):
        if c.reply() != val:
            ok = False
            break
    check(ok, "40 queued 200 KB replies read slowly arrive intact")
    c.close()

    # Same, but the client half-closes right after sending: the server sees EOF while most of
    # the replies are still queued, and must keep sending until they are all delivered.
    c = srv.conn()
    c.s.sendall(enc("GET", "slow") * 40)
    c.s.shutdown(socket.SHUT_WR)
    time.sleep(0.5)
    ok = True
    for _ in range(40):
        if c.reply() != val:
            ok = False
            break
    check(ok, "queued replies still delivered after the client half-closed")
    check(c.closed_by_server(), "then the server closes")
    c.close()


def main():
    global failures
    srv = Server()
    try:
        for t in (test_basic, test_partial_reads, test_pipelining, test_malformed, test_sizes,
                  test_disconnects, test_expiry, test_many_clients, test_slow_reader):
            t0 = time.time()
            n0, f0 = checks, failures
            t(srv)
            print(f"  {t.__name__:<22} {checks - n0:4d} checks, {failures - f0} failed "
                  f"({time.time() - t0:.1f}s)")
        check(srv.alive(), "server still running at the end")
    finally:
        err = b""
        if srv.alive():
            srv.stop()
        else:
            err = srv.proc.stderr.read()
            print("SERVER DIED. stderr:", err.decode(errors="replace"))
            failures += 1
    test_eviction()
    print(f"test_protocol: {checks - failures} of {checks} checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
