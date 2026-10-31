#pragma once
// RESP (REdis Serialization Protocol) parsing and reply encoding.
//
// A client command is an array of bulk strings:
//   *3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n
// Replies are one of: +simple, -error, :integer, $bulk, or $-1 (nil).

#include <cstddef>
#include <string>
#include <vector>

// Limits. A bad client must not be able to make the server allocate or wait forever.
constexpr size_t kMaxBulkLen = 1 << 20;  // largest single argument: 1 MiB
constexpr size_t kMaxArgs = 8;           // most arguments in one command
constexpr size_t kMaxHeaderLine = 32;    // longest "*N" or "$N" line we will scan

enum class ParseStatus {
    Ok,        // one complete command was parsed
    NeedMore,  // the bytes so far are valid but incomplete: wait for more
    Error,     // the bytes are not valid RESP: reply with an error and close
};

struct ParseResult {
    ParseStatus status = ParseStatus::NeedMore;
    size_t consumed = 0;            // bytes used by the command (only when Ok)
    std::vector<std::string> args;  // the command's arguments (only when Ok)
};

// Look at the first `len` bytes at `data` and try to parse ONE command.
// Never reads past `len`. Safe to call again with a longer buffer after NeedMore.
ParseResult parse_command(const char* data, size_t len);

// Reply encoders: each appends to `out`.
void append_simple(std::string& out, const std::string& s);  // +OK\r\n
void append_error(std::string& out, const std::string& s);   // -ERR ...\r\n
void append_int(std::string& out, long long v);              // :42\r\n
void append_bulk(std::string& out, const std::string& s);    // $3\r\nfoo\r\n
void append_nil(std::string& out);                           // $-1\r\n
