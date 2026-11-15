#include "resp.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using resp::parse_command;
using resp::Status;

TEST_CASE("parses a well-formed SET command", "[resp]") {
    const std::string buf = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    const auto result = parse_command(buf);

    REQUIRE(result.status == Status::Ok);
    REQUIRE(result.consumed == buf.size());
    REQUIRE(result.args == std::vector<std::string>{"SET", "foo", "bar"});
}

TEST_CASE("every prefix of a well-formed command stays Incomplete with no args", "[resp]") {
    const std::string buf = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";

    for (size_t len = 0; len < buf.size(); ++len) {
        INFO("prefix length = " << len);
        const auto result = parse_command(buf.substr(0, len));
        REQUIRE(result.status == Status::Incomplete);
        REQUIRE(result.args.empty());
    }

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

TEST_CASE("parses an empty bulk string as a single empty arg", "[resp]") {
    const std::string buf = "*1\r\n$0\r\n\r\n";
    const auto result = parse_command(buf);

    REQUIRE(result.status == Status::Ok);
    REQUIRE(result.consumed == buf.size());
    REQUIRE(result.args == std::vector<std::string>{""});
}

TEST_CASE("pipelined commands are parsed one at a time by advancing past consumed",
          "[resp]") {
    const std::string first = "*1\r\n$4\r\nPING\r\n";
    const std::string second = "*1\r\n$4\r\nPONG\r\n";
    const std::string buf = first + second;

    const auto first_result = parse_command(buf);
    REQUIRE(first_result.status == Status::Ok);
    REQUIRE(first_result.consumed == first.size());
    REQUIRE(first_result.args == std::vector<std::string>{"PING"});

    // This is the loop the server will actually run: feed back whatever is
    // left after consumed and keep parsing until the buffer runs dry.
    const auto second_result = parse_command(std::string_view(buf).substr(first_result.consumed));
    REQUIRE(second_result.status == Status::Ok);
    REQUIRE(second_result.consumed == second.size());
    REQUIRE(second_result.args == std::vector<std::string>{"PONG"});
}

TEST_CASE("bulk data is treated as raw bytes, not line-scanned", "[resp]") {
    // The payload itself contains a "\r\n" -- the parser must trust the
    // declared length instead of searching for a terminator inside the data.
    const std::string buf = "*1\r\n$5\r\nab\r\nc\r\n";
    const auto result = parse_command(buf);

    REQUIRE(result.status == Status::Ok);
    REQUIRE(result.args == std::vector<std::string>{"ab\r\nc"});
}

TEST_CASE("bulk data may contain embedded NUL bytes", "[resp]") {
    // A plain string literal would truncate at the embedded '\0', so the
    // length must be passed explicitly: "*1\r\n" (4) + "$3\r\n" (4) +
    // "a\0b" (3, matching the declared $3 length) + "\r\n" (2) = 13 bytes.
    const std::string buf("*1\r\n$3\r\na\0b\r\n", 13);
    const auto result = parse_command(buf);

    REQUIRE(result.status == Status::Ok);
    REQUIRE(result.consumed == buf.size());
    REQUIRE(result.args == std::vector<std::string>{std::string("a\0b", 3)});
}

void require_empty_result(const resp::ParseResult& result, Status expected_status) {
    REQUIRE(result.status == expected_status);
    REQUIRE(result.consumed == 0);
    REQUIRE(result.args.empty());
}

TEST_CASE("incomplete buffers are reported as Incomplete, not Error", "[resp]") {
    SECTION("empty buffer") {
        require_empty_result(parse_command(""), Status::Incomplete);
    }

    SECTION("count line not yet terminated") {
        require_empty_result(parse_command("*3"), Status::Incomplete);
    }

    SECTION("count line complete, nothing after it") {
        require_empty_result(parse_command("*1\r\n"), Status::Incomplete);
    }

    SECTION("bulk header not yet terminated") {
        require_empty_result(parse_command("*1\r\n$3"), Status::Incomplete);
    }

    SECTION("bulk data not fully arrived") {
        require_empty_result(parse_command("*1\r\n$3\r\nfo"), Status::Incomplete);
    }

    SECTION("bulk data arrived but trailing CRLF hasn't") {
        require_empty_result(parse_command("*1\r\n$3\r\nfoo\r"), Status::Incomplete);
    }

    SECTION("arg count exactly at the max is accepted, not rejected") {
        // Exercises the reserve() fix: with no bytes left for any argument,
        // this must not try to reserve capacity for 1,000,000 strings.
        require_empty_result(parse_command("*1000000\r\n"), Status::Incomplete);
    }

    SECTION("bulk length exactly at the max is accepted, not rejected") {
        // No 512 MB allocation should happen -- we return Incomplete before
        // ever reaching the point where argument bytes get copied.
        require_empty_result(parse_command("*1\r\n$536870912\r\n"), Status::Incomplete);
    }
}

TEST_CASE("malformed frames are rejected as Error", "[resp]") {
    SECTION("does not start with *") {
        require_empty_result(parse_command("PING\r\n"), Status::Error);
    }

    SECTION("count is not numeric") {
        require_empty_result(parse_command("*x\r\n"), Status::Error);
    }

    SECTION("count has trailing junk") {
        require_empty_result(parse_command("*3x\r\n"), Status::Error);
    }

    SECTION("count is negative") {
        require_empty_result(parse_command("*-1\r\n"), Status::Error);
    }

    SECTION("count exceeds the max arg count") {
        require_empty_result(parse_command("*1000001\r\n"), Status::Error);
    }

    SECTION("bulk header missing $ prefix") {
        require_empty_result(parse_command("*1\r\n+3\r\nfoo\r\n"), Status::Error);
    }

    SECTION("bulk length is not numeric") {
        require_empty_result(parse_command("*1\r\n$x\r\n"), Status::Error);
    }

    SECTION("bulk length has trailing junk") {
        require_empty_result(parse_command("*1\r\n$3x\r\n"), Status::Error);
    }

    SECTION("bulk length is negative") {
        // $-1 is a valid null-bulk marker in replies, but not inside a request.
        require_empty_result(parse_command("*1\r\n$-1\r\n"), Status::Error);
    }

    SECTION("bulk length exceeds the max bulk length") {
        require_empty_result(parse_command("*1\r\n$536870913\r\n"), Status::Error);
    }

    SECTION("bulk data not followed by CRLF") {
        require_empty_result(parse_command("*1\r\n$3\r\nfooXX"), Status::Error);
    }
}

TEST_CASE("oversized header lines are rejected outright, not held as incomplete", "[resp]") {
    SECTION("count line with no CRLF but already longer than any real header") {
        const std::string buf = "*" + std::string(100, '9');
        require_empty_result(parse_command(buf), Status::Error);
    }

    SECTION("count line terminated but longer than the header-line bound") {
        const std::string buf = "*" + std::string(100, '9') + "\r\n";
        require_empty_result(parse_command(buf), Status::Error);
    }

    SECTION("bulk length line with no CRLF but already too long") {
        const std::string buf = "*1\r\n$" + std::string(100, '9');
        require_empty_result(parse_command(buf), Status::Error);
    }
}
