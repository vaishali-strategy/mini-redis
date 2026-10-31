#include <random>
#include <string>

#include "check.hpp"
#include "resp.hpp"

static ParseResult parse(const std::string& s) { return parse_command(s.data(), s.size()); }

static std::string cmd(std::initializer_list<std::string> parts) {
    std::string s = "*" + std::to_string(parts.size()) + "\r\n";
    for (const auto& p : parts) s += "$" + std::to_string(p.size()) + "\r\n" + p + "\r\n";
    return s;
}

int main() {
    // A complete command.
    {
        std::string wire = cmd({"SET", "a", "1"});
        ParseResult r = parse(wire);
        CHECK(r.status == ParseStatus::Ok);
        CHECK(r.consumed == wire.size());
        CHECK(r.args.size() == 3);
        CHECK(r.args[0] == "SET" && r.args[1] == "a" && r.args[2] == "1");
    }

    // Fed one byte at a time: every proper prefix is NeedMore, the whole thing is Ok.
    {
        std::string wire = cmd({"SET", "key", "some value"});
        for (size_t n = 0; n < wire.size(); ++n)
            CHECK(parse_command(wire.data(), n).status == ParseStatus::NeedMore);
        CHECK(parse_command(wire.data(), wire.size()).status == ParseStatus::Ok);
    }

    // Two commands in one buffer: consumed tells us where the second starts.
    {
        std::string a = cmd({"GET", "x"});
        std::string b = cmd({"DEL", "x", "y"});
        std::string wire = a + b;
        ParseResult r1 = parse(wire);
        CHECK(r1.status == ParseStatus::Ok && r1.consumed == a.size());
        ParseResult r2 = parse_command(wire.data() + r1.consumed, wire.size() - r1.consumed);
        CHECK(r2.status == ParseStatus::Ok && r2.consumed == b.size());
        CHECK(r2.args.size() == 3 && r2.args[2] == "y");
    }

    // Leftover bytes after a command are not touched.
    {
        std::string a = cmd({"PING"});
        ParseResult r = parse(a + "*1\r\n$4\r\nPI");
        CHECK(r.status == ParseStatus::Ok && r.consumed == a.size());
    }

    // An empty argument, and an argument containing \r\n (bulk strings are length-based).
    {
        ParseResult r = parse(cmd({"SET", "k", ""}));
        CHECK(r.status == ParseStatus::Ok && r.args[2].empty());
        std::string tricky = "a\r\nb\r\n";
        r = parse(cmd({"SET", "k", tricky}));
        CHECK(r.status == ParseStatus::Ok && r.args[2] == tricky);
        std::string binary("x\0y", 3);
        r = parse(cmd({"SET", "k", binary}));
        CHECK(r.status == ParseStatus::Ok && r.args[2] == binary);
    }

    // Invalid input must be Error, not NeedMore, so the server can reject it.
    CHECK(parse("X\r\n").status == ParseStatus::Error);                    // not '*'
    CHECK(parse("PING\r\n").status == ParseStatus::Error);                 // inline command
    CHECK(parse("*0\r\n").status == ParseStatus::Error);                   // empty array
    CHECK(parse("*-1\r\n").status == ParseStatus::Error);                  // negative count
    CHECK(parse("*9\r\n").status == ParseStatus::Error);                   // over kMaxArgs
    CHECK(parse("*a\r\n").status == ParseStatus::Error);                   // not a number
    CHECK(parse("*\r\n").status == ParseStatus::Error);                    // no digits
    CHECK(parse("*1\r\n$-1\r\n").status == ParseStatus::Error);            // negative length
    CHECK(parse("*1\r\n$abc\r\n").status == ParseStatus::Error);
    CHECK(parse("*1\r\n+OK\r\n").status == ParseStatus::Error);            // wrong element type
    CHECK(parse("*1\r\n$3\r\nabcXX").status == ParseStatus::Error);        // body not followed by CRLF
    CHECK(parse("*1\r\r").status == ParseStatus::Error);                   // '\r' not followed by '\n'
    CHECK(parse("*99999999999999999999\r\n").status == ParseStatus::Error);  // overflow attempt
    CHECK(parse(std::string("*1") + std::string(100, '1')).status == ParseStatus::Error);  // endless header

    // Length limits: checked BEFORE waiting for the body, so a lie costs nothing.
    {
        std::string over = "*1\r\n$" + std::to_string(kMaxBulkLen + 1) + "\r\n";
        CHECK(parse(over).status == ParseStatus::Error);
        std::string at_limit = "*1\r\n$" + std::to_string(kMaxBulkLen) + "\r\n";
        CHECK(parse(at_limit).status == ParseStatus::NeedMore);  // legal, body just not here yet
    }

    // A header split in the middle of "\r\n" is NeedMore, not Error.
    CHECK(parse("*1\r").status == ParseStatus::NeedMore);
    CHECK(parse("*1\r\n$3\r").status == ParseStatus::NeedMore);
    CHECK(parse("").status == ParseStatus::NeedMore);

    // Reply encoders.
    {
        std::string out;
        append_simple(out, "OK");
        append_error(out, "ERR boom");
        append_int(out, -2);
        append_bulk(out, "foo");
        append_nil(out);
        CHECK(out == "+OK\r\n-ERR boom\r\n:-2\r\n$3\r\nfoo\r\n$-1\r\n");
    }

    // Fuzz: random bytes must never crash, hang, or claim to consume more than they were given.
    {
        std::mt19937 rng(12345);
        const std::string alphabet = "*$0123456789\r\n-+abXZ ";
        for (int i = 0; i < 20000; ++i) {
            std::string s;
            size_t len = rng() % 40;
            for (size_t j = 0; j < len; ++j) s += alphabet[rng() % alphabet.size()];
            ParseResult r = parse(s);
            if (r.status == ParseStatus::Ok) {
                CHECK(r.consumed <= s.size() && !r.args.empty());
            }
        }
    }

    return finish("test_resp");
}
