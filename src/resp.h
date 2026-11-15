#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace resp {

enum class Status {
    Ok,          // A full command was parsed; see ParseResult::args.
    Incomplete,  // Not enough bytes yet; call again once more data arrives.
    Error,       // The buffer does not contain a valid RESP command.
};

struct ParseResult {
    Status status = Status::Incomplete;

    // Number of bytes consumed from the front of buf. Only meaningful when
    // status is Ok -- the caller should erase/advance past this many bytes
    // before parsing the next command. On Error or Incomplete this is always
    // 0; a malformed buffer has no well-defined resync point, so the caller
    // should drop the connection rather than try to skip past the bad data.
    size_t consumed = 0;

    // The parsed command's arguments. When status is Ok and args is empty
    // (i.e. the client sent "*0\r\n"), that's a valid empty command -- skip
    // it rather than treating it as something to execute or as an error.
    std::vector<std::string> args;
};

ParseResult parse_command(std::string_view buf);

}  // namespace resp
