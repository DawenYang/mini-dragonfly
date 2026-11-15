#include "resp.h"

#include <charconv>
#include <cstddef>
#include <string_view>
#include <system_error>

namespace resp {
namespace {
constexpr long long kMaxArgCount = 1'000'000;
constexpr long long kMaxBulkLen = 512LL * 1024 * 1024;
constexpr size_t kMaxHeaderLineLen = 64;

bool read_line(std::string_view buf, size_t pos, std::string_view& line, size_t& next_pos) {
    const size_t crlf = buf.find("\r\n", pos);
    if (crlf == std::string_view::npos) {
        return false;
    }
    line = buf.substr(pos, crlf - pos);
    next_pos = crlf + 2;
    return true;
}

bool read_bounded_line(std::string_view buf, size_t pos, std::string_view& line, size_t& next_pos,
                       Status& fail_status) {
    if (!read_line(buf, pos, line, next_pos)) {
        fail_status = (buf.size() - pos > kMaxHeaderLineLen) ? Status::Error : Status::Incomplete;
        return false;
    }
    if (line.size() > kMaxHeaderLineLen) {
        fail_status = Status::Error;
        return false;
    }
    return true;
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

    if (!read_bounded_line(buf, pos, line, next_pos, result.status)) {
        return result;
    }

    long long arg_count = 0;
    if (!to_int(line, arg_count) || arg_count < 0 || arg_count > kMaxArgCount) {
        result.status = Status::Error;
        return result;
    }

    pos = next_pos;
    result.args.reserve(static_cast<size_t>(arg_count));

    for (long long i = 0; i < arg_count; ++i) {
        if (!read_bounded_line(buf, pos, line, next_pos, result.status)) {
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

        result.args.emplace_back(buf.substr(pos, static_cast<size_t>(bulk_len)));
        pos += required;
    }

    result.status = Status::Ok;
    result.consumed = pos;
    return result;
}
}  // namespace resp
