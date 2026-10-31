#include "commands.hpp"

#include <cctype>

#include "resp.hpp"

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Strict integer parse: optional sign, then 1-18 digits, nothing else.
bool parse_int(const std::string& s, long long& out) {
    if (s.empty() || s.size() > 18) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i == s.size()) return false;
    long long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    out = (s[0] == '-') ? -v : v;
    return true;
}

void wrong_arity(std::string& out, const std::string& name) {
    std::string lower;
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    append_error(out, "ERR wrong number of arguments for '" + lower + "' command");
}

}  // namespace

void execute_command(Store& store, const std::vector<std::string>& args, std::string& out) {
    const std::string cmd = upper(args[0]);
    const size_t n = args.size();

    if (cmd == "PING") {
        if (n == 1)
            append_simple(out, "PONG");
        else if (n == 2)
            append_bulk(out, args[1]);
        else
            wrong_arity(out, cmd);

    } else if (cmd == "SET") {
        if (n != 3) {
            wrong_arity(out, cmd);
            return;
        }
        store.set(args[1], args[2]);
        append_simple(out, "OK");

    } else if (cmd == "GET") {
        if (n != 2) {
            wrong_arity(out, cmd);
            return;
        }
        const std::string* v = store.get(args[1]);
        if (v)
            append_bulk(out, *v);
        else
            append_nil(out);

    } else if (cmd == "DEL") {
        if (n < 2) {
            wrong_arity(out, cmd);
            return;
        }
        long long removed = 0;
        for (size_t i = 1; i < n; ++i)
            if (store.del(args[i])) ++removed;
        append_int(out, removed);

    } else if (cmd == "EXPIRE") {
        if (n != 3) {
            wrong_arity(out, cmd);
            return;
        }
        long long seconds = 0;
        if (!parse_int(args[2], seconds)) {
            append_error(out, "ERR value is not an integer or out of range");
            return;
        }
        if (seconds > kMaxTtlSeconds) {
            append_error(out, "ERR invalid expire time in 'expire' command");
            return;
        }
        append_int(out, store.expire(args[1], seconds) ? 1 : 0);

    } else if (cmd == "TTL") {
        if (n != 2) {
            wrong_arity(out, cmd);
            return;
        }
        append_int(out, store.ttl(args[1]));

    } else if (cmd == "DBSIZE") {
        if (n != 1) {
            wrong_arity(out, cmd);
            return;
        }
        append_int(out, static_cast<long long>(store.size()));

    } else {
        // Echo the name back, but a '\r' or '\n' inside it would break the reply framing.
        std::string shown;
        for (char c : args[0]) shown += (c == '\r' || c == '\n') ? ' ' : c;
        append_error(out, "ERR unknown command '" + shown + "'");
    }
}
