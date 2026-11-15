#include "resp.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

using resp::parse_command;
using resp::Status;

TEST_CASE("parses a well-formed SET command", "[resp]") {
    const std::string buf = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    const auto result = parse_command(buf);

    REQUIRE(result.status == Status::Ok);
    REQUIRE(result.consumed == buf.size());
    REQUIRE(result.args == std::vector<std::string>{"SET", "foo", "bar"});
}

TEST_CASE("parses a zero-arg command", "[resp]") {
    const std::string buf = "*0\r\n";
    const auto result = parse_command(buf);

    REQUIRE(result.status == Status::Ok);
    REQUIRE(result.consumed == buf.size());
    REQUIRE(result.args.empty());
}

TEST_CASE("only consumes the first command from a pipelined buffer", "[resp]") {
    const std::string first = "*1\r\n$4\r\nPING\r\n";
    const std::string buf = first + "*1\r\n$4\r\nPONG\r\n";
    const auto result = parse_command(buf);

    REQUIRE(result.status == Status::Ok);
    REQUIRE(result.consumed == first.size());
    REQUIRE(result.args == std::vector<std::string>{"PING"});
}

TEST_CASE("bulk data is treated as raw bytes, not line-scanned", "[resp]") {
    // The payload itself contains a "\r\n" -- the parser must trust the
    // declared length instead of searching for a terminator inside the data.
    const std::string buf = "*1\r\n$5\r\nab\r\nc\r\n";
    const auto result = parse_command(buf);

    REQUIRE(result.status == Status::Ok);
    REQUIRE(result.args == std::vector<std::string>{"ab\r\nc"});
}

TEST_CASE("incomplete buffers are reported as Incomplete, not Error", "[resp]") {
    SECTION("empty buffer") {
        REQUIRE(parse_command("").status == Status::Incomplete);
    }

    SECTION("count line not yet terminated") {
        REQUIRE(parse_command("*3").status == Status::Incomplete);
    }

    SECTION("count line complete, nothing after it") {
        REQUIRE(parse_command("*1\r\n").status == Status::Incomplete);
    }

    SECTION("bulk header not yet terminated") {
        REQUIRE(parse_command("*1\r\n$3").status == Status::Incomplete);
    }

    SECTION("bulk data not fully arrived") {
        REQUIRE(parse_command("*1\r\n$3\r\nfo").status == Status::Incomplete);
    }

    SECTION("bulk data arrived but trailing CRLF hasn't") {
        REQUIRE(parse_command("*1\r\n$3\r\nfoo\r").status == Status::Incomplete);
    }
}

TEST_CASE("malformed frames are rejected as Error", "[resp]") {
    SECTION("does not start with *") {
        REQUIRE(parse_command("PING\r\n").status == Status::Error);
    }

    SECTION("count is not numeric") {
        REQUIRE(parse_command("*x\r\n").status == Status::Error);
    }

    SECTION("count has trailing junk") {
        REQUIRE(parse_command("*3x\r\n").status == Status::Error);
    }

    SECTION("count is negative") {
        REQUIRE(parse_command("*-1\r\n").status == Status::Error);
    }

    SECTION("count exceeds the max arg count") {
        REQUIRE(parse_command("*1000001\r\n").status == Status::Error);
    }

    SECTION("bulk header missing $ prefix") {
        REQUIRE(parse_command("*1\r\n+3\r\nfoo\r\n").status == Status::Error);
    }

    SECTION("bulk length is not numeric") {
        REQUIRE(parse_command("*1\r\n$x\r\n").status == Status::Error);
    }

    SECTION("bulk length has trailing junk") {
        REQUIRE(parse_command("*1\r\n$3x\r\n").status == Status::Error);
    }

    SECTION("bulk length is negative") {
        // $-1 is a valid null-bulk marker in replies, but not inside a request.
        REQUIRE(parse_command("*1\r\n$-1\r\n").status == Status::Error);
    }

    SECTION("bulk length exceeds the max bulk length") {
        REQUIRE(parse_command("*1\r\n$536870913\r\n").status == Status::Error);
    }

    SECTION("bulk data not followed by CRLF") {
        REQUIRE(parse_command("*1\r\n$3\r\nfooXX").status == Status::Error);
    }
}

TEST_CASE("oversized header lines are rejected outright, not held as incomplete", "[resp]") {
    SECTION("count line with no CRLF but already longer than any real header") {
        const std::string buf = "*" + std::string(100, '9');
        REQUIRE(parse_command(buf).status == Status::Error);
    }

    SECTION("count line terminated but longer than the header-line bound") {
        const std::string buf = "*" + std::string(100, '9') + "\r\n";
        REQUIRE(parse_command(buf).status == Status::Error);
    }

    SECTION("bulk length line with no CRLF but already too long") {
        const std::string buf = "*1\r\n$" + std::string(100, '9');
        REQUIRE(parse_command(buf).status == Status::Error);
    }
}
