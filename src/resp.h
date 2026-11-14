#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace resp {

enum class Status { Ok, Incomplete, Error };

struct ParseResult {
    Status status = Status::Incomplete;
    size_t consumed = 0;
    std::vector<std::string> args;
};

ParseResult parse_command(std::string_view buf);

}  // namespace resp
