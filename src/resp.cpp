#include "resp.hpp"

namespace {

enum class Line { Ok, NeedMore, Bad };

// Parse a header line "<prefix><digits>\r\n" that starts at data[pos].
// On Ok: `value` holds the number and `pos` is moved past the "\r\n".
// The value must be <= max_value, which also stops digit strings from overflowing.
Line read_header(const char* data, size_t len, size_t& pos, char prefix, size_t max_value,
                 size_t& value) {
    if (pos >= len) return Line::NeedMore;
    if (data[pos] != prefix) return Line::Bad;

    size_t i = pos + 1;
    size_t digits = 0;
    size_t v = 0;
    while (true) {
        if (i >= len) return Line::NeedMore;
        char ch = data[i];
        if (ch == '\r') break;
        if (ch < '0' || ch > '9') return Line::Bad;  // also rejects "-1"
        if (++digits > kMaxHeaderLine) return Line::Bad;
        v = v * 10 + static_cast<size_t>(ch - '0');
        if (v > max_value) return Line::Bad;
        ++i;
    }
    if (digits == 0) return Line::Bad;
    if (i + 1 >= len) return Line::NeedMore;  // have the '\r', still waiting for '\n'
    if (data[i + 1] != '\n') return Line::Bad;

    value = v;
    pos = i + 2;
    return Line::Ok;
}

ParseResult make(ParseStatus s) {
    ParseResult r;
    r.status = s;
    return r;
}

}  // namespace

ParseResult parse_command(const char* data, size_t len) {
    size_t pos = 0;

    size_t argc = 0;
    Line h = read_header(data, len, pos, '*', kMaxArgs, argc);
    if (h == Line::NeedMore) return make(ParseStatus::NeedMore);
    if (h == Line::Bad || argc == 0) return make(ParseStatus::Error);

    // First pass: walk the structure and remember where each argument is.
    // Nothing is copied until we know the whole command is here.
    struct Span {
        size_t start;
        size_t length;
    };
    std::vector<Span> spans;
    spans.reserve(argc);

    for (size_t i = 0; i < argc; ++i) {
        size_t blen = 0;
        h = read_header(data, len, pos, '$', kMaxBulkLen, blen);
        if (h == Line::NeedMore) return make(ParseStatus::NeedMore);
        if (h == Line::Bad) return make(ParseStatus::Error);

        if (len - pos < blen + 2) return make(ParseStatus::NeedMore);  // body + "\r\n"
        if (data[pos + blen] != '\r' || data[pos + blen + 1] != '\n')
            return make(ParseStatus::Error);

        spans.push_back({pos, blen});
        pos += blen + 2;
    }

    ParseResult r;
    r.status = ParseStatus::Ok;
    r.consumed = pos;
    r.args.reserve(argc);
    for (const Span& s : spans) r.args.emplace_back(data + s.start, s.length);
    return r;
}

void append_simple(std::string& out, const std::string& s) {
    out += '+';
    out += s;
    out += "\r\n";
}

void append_error(std::string& out, const std::string& s) {
    out += '-';
    out += s;
    out += "\r\n";
}

void append_int(std::string& out, long long v) {
    out += ':';
    out += std::to_string(v);
    out += "\r\n";
}

void append_bulk(std::string& out, const std::string& s) {
    out += '$';
    out += std::to_string(s.size());
    out += "\r\n";
    out += s;
    out += "\r\n";
}

void append_nil(std::string& out) { out += "$-1\r\n"; }
