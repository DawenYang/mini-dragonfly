#pragma once

#include <charconv>
#include <string_view>
#include <system_error>

namespace mini_dragonfly {

// Parses s as a 64-bit integer. Rejects empty strings and any trailing
// junk after the number -- std::from_chars alone accepts a valid prefix
// and silently ignores the rest (e.g. "12abc" would parse as 12).
//
// Shared across modules with the same need: the RESP header-length
// parser (resp.cpp), a command argument that must be an integer (e.g.
// EXPIRE's seconds in server.cpp), and a stored value being read back as
// one (INCR in store.hh).
inline bool parse_integer(std::string_view s, long long& out) {
    if (s.empty()) {
        return false;
    }
    const auto* begin = s.data();
    const auto* end = s.data() + s.size();
    const auto parsed = std::from_chars(begin, end, out);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

}  // namespace mini_dragonfly
