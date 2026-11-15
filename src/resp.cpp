#include "resp.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace resp {
namespace {
constexpr long long kMaxArgCount = 1'000'000;
constexpr long long kMaxBulkLen = 512LL * 1024 * 1024;
constexpr size_t kMaxHeaderLineLen = 64;

// Searches at most max_len + 2 bytes starting at pos, so a header with no CRLF
// followed by megabytes of junk can't force an unbounded scan.
bool read_line(std::string_view buf, size_t pos, size_t max_len, std::string_view& line,
               size_t& next_pos) {
    const size_t window_len = std::min(buf.size() - pos, max_len + 2);
    const std::string_view window = buf.substr(pos, window_len);
    const size_t crlf = window.find("\r\n");
    if (crlf == std::string_view::npos) {
        return false;
    }
    line = window.substr(0, crlf);
    next_pos = pos + crlf + 2;
    return true;
}

Status read_bounded_line(std::string_view buf, size_t pos, std::string_view& line,
                         size_t& next_pos) {
    if (!read_line(buf, pos, kMaxHeaderLineLen, line, next_pos)) {
        return (buf.size() - pos > kMaxHeaderLineLen) ? Status::Error : Status::Incomplete;
    }
    return Status::Ok;
}

bool to_int(std::string_view s, long long& out) {
    if (s.empty()) {
        return false;
    }

    long long value = 0;
    const auto* begin = s.data();
    const auto* end = s.data() + s.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return false;
    }
    out = value;
    return true;
}
}  // namespace

ParseResult parse_command(std::string_view buf) {
    ParseResult result;

    if (buf.empty()) {
        result.status = Status::Incomplete;
        return result;
    }

    if (buf.front() != '*') {
        result.status = Status::Error;
        return result;
    }

    size_t pos = 1;
    std::string_view line;
    size_t next_pos = 0;

    result.status = read_bounded_line(buf, pos, line, next_pos);
    if (result.status != Status::Ok) {
        return result;
    }

    long long arg_count = 0;
    if (!to_int(line, arg_count) || arg_count < 0 || arg_count > kMaxArgCount) {
        result.status = Status::Error;
        return result;
    }

    pos = next_pos;

    // First pass: validate structure and record each argument's (offset, length)
    // without copying any bytes. A command that arrives over many small chunks
    // gets re-validated on every call, but that's cheap arithmetic over header
    // bytes -- unlike copying argument data, which we now only ever do once,
    // on the call that finds the command complete.
    std::vector<std::pair<size_t, size_t>> spans;
    spans.reserve(std::min<size_t>(static_cast<size_t>(arg_count), (buf.size() - pos) / 6));

    for (long long i = 0; i < arg_count; ++i) {
        result.status = read_bounded_line(buf, pos, line, next_pos);
        if (result.status != Status::Ok) {
            return result;
        }

        if (line.empty() || line.front() != '$') {
            result.status = Status::Error;
            return result;
        }

        long long bulk_len = 0;
        if (!to_int(line.substr(1), bulk_len) || bulk_len < 0 || bulk_len > kMaxBulkLen) {
            result.status = Status::Error;
            return result;
        }

        pos = next_pos;
        const size_t required = static_cast<size_t>(bulk_len) + 2;
        if (buf.size() - pos < required) {
            result.status = Status::Incomplete;
            return result;
        }

        if (buf[pos + static_cast<size_t>(bulk_len)] != '\r' ||
            buf[pos + static_cast<size_t>(bulk_len) + 1] != '\n') {
            result.status = Status::Error;
            return result;
        }

        spans.emplace_back(pos, static_cast<size_t>(bulk_len));
        pos += required;
    }

    // Second pass: the command is confirmed complete, so now (and only now)
    // copy the argument bytes.
    result.args.reserve(spans.size());
    for (const auto& [offset, length] : spans) {
        result.args.emplace_back(buf.substr(offset, length));
    }

    result.status = Status::Ok;
    result.consumed = pos;
    return result;
}
}  // namespace resp
