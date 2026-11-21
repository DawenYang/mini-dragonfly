#include "server.hh"

#include "resp.h"

#include <seastar/core/loop.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/shard_id.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/log.hh>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mini_dragonfly {

namespace {

seastar::logger applog("server");

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string encode_bulk(const std::optional<std::string>& value) {
    if (!value) {
        return "$-1\r\n";
    }
    return "$" + std::to_string(value->size()) + "\r\n" + *value + "\r\n";
}

std::string encode_integer(long long value) { return ":" + std::to_string(value) + "\r\n"; }

// Which shard owns a given key. Lives here (not in ShardStore) since it's
// routing logic, not storage logic -- keeps ShardStore itself free of any
// Seastar dependency so it can be unit-tested without a reactor.
unsigned owner_shard(const std::string& key) {
    return static_cast<unsigned>(std::hash<std::string>{}(key) % seastar::smp::count);
}

// Parses a RESP bulk-string argument as an integer. Rejects empty strings
// and trailing junk, same contract as resp::to_int (duplicated here rather
// than exposed from resp.cpp, since that's an internal parsing detail, not
// part of the RESP module's public API).
bool parse_integer_arg(const std::string& s, long long& out) {
    if (s.empty()) {
        return false;
    }
    const auto* begin = s.data();
    const auto* end = s.data() + s.size();
    const auto parsed = std::from_chars(begin, end, out);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

class Connection {
public:
    Connection(seastar::connected_socket socket, seastar::sharded<ShardStore>& store)
        : socket_(std::move(socket)), in_(socket_.input()), out_(socket_.output()), store_(store) {}

    seastar::future<> handle() {
        return seastar::do_until([this] { return stop_; }, [this] { return process_one(); })
            .finally([this] { return out_.close(); });
    }

private:
    // Tries to parse a command out of whatever's already buffered. If that's
    // not enough, reads more and retries. Resolves with a ParseResult whose
    // status is Ok or Error once a definitive answer is known, or Incomplete
    // if eof was reached while a command was still incomplete.
    seastar::future<resp::ParseResult> parse_next_command() {
        auto result = resp::parse_command(buf_);
        if (result.status != resp::Status::Incomplete) {
            return seastar::make_ready_future<resp::ParseResult>(std::move(result));
        }
        return seastar::repeat_until_value([this] { return read_more_or_done(); });
    }

    seastar::future<std::optional<resp::ParseResult>> read_more_or_done() {
        return in_.read().then([this](seastar::temporary_buffer<char> chunk) {
            if (chunk.empty()) {
                resp::ParseResult eof_result;
                eof_result.status = resp::Status::Incomplete;
                return std::optional<resp::ParseResult>(std::move(eof_result));
            }
            buf_.append(chunk.get(), chunk.size());
            auto result = resp::parse_command(buf_);
            if (result.status != resp::Status::Incomplete) {
                return std::optional<resp::ParseResult>(std::move(result));
            }
            return std::optional<resp::ParseResult>(std::nullopt);
        });
    }

    seastar::future<> process_one() {
        return parse_next_command().then([this](resp::ParseResult result) {
            if (result.status == resp::Status::Incomplete) {
                // Connection closed mid-command; nothing to respond to.
                stop_ = true;
                return seastar::make_ready_future<>();
            }
            if (result.status == resp::Status::Error) {
                stop_ = true;
                return out_.write("-ERR protocol error\r\n").then([this] { return out_.flush(); });
            }
            buf_.erase(0, result.consumed);
            if (result.args.empty()) {
                // "*0\r\n": a valid empty command. Nothing to execute or reply to.
                return seastar::make_ready_future<>();
            }
            return dispatch(std::move(result.args));
        });
    }

    seastar::future<> dispatch(std::vector<std::string> args) {
        auto cmd = to_upper(args[0]);
        if (cmd == "GET" && args.size() == 2) {
            return do_get(args[1]);
        }
        if (cmd == "SET" && args.size() == 3) {
            return do_set(args[1], args[2]);
        }
        if (cmd == "EXPIRE" && args.size() == 3) {
            return do_expire(args[1], args[2]);
        }
        return out_.write("-ERR unknown command or wrong number of arguments\r\n").then([this] {
            return out_.flush();
        });
    }

    // Routes to whichever shard owns this key via Seastar's own cross-shard
    // messaging (sharded<T>::invoke_on, backed by smp_message_queue). If
    // that happens to be this shard, Seastar calls through directly without
    // going through the queue at all.
    seastar::future<> do_get(const std::string& key) {
        return store_.invoke_on(owner_shard(key), [key](ShardStore& s) { return s.get(key); })
            .then([this](std::optional<std::string> value) {
                return out_.write(encode_bulk(value)).then([this] { return out_.flush(); });
            });
    }

    seastar::future<> do_set(const std::string& key, const std::string& value) {
        return store_.invoke_on(owner_shard(key), [key, value](ShardStore& s) { s.set(key, value); })
            .then([this] { return out_.write("+OK\r\n").then([this] { return out_.flush(); }); });
    }

    seastar::future<> do_expire(const std::string& key, const std::string& seconds_str) {
        long long seconds = 0;
        if (!parse_integer_arg(seconds_str, seconds)) {
            return out_.write("-ERR value is not an integer or out of range\r\n").then([this] {
                return out_.flush();
            });
        }
        return store_.invoke_on(owner_shard(key),
                                 [key, seconds](ShardStore& s) { return s.expire(key, seconds); })
            .then([this](bool did_set) {
                return out_.write(encode_integer(did_set ? 1 : 0)).then([this] { return out_.flush(); });
            });
    }

    seastar::connected_socket socket_;
    seastar::input_stream<char> in_;
    seastar::output_stream<char> out_;
    seastar::sharded<ShardStore>& store_;
    std::string buf_;
    bool stop_ = false;
};

}  // namespace

TcpServer::TcpServer(uint16_t port, seastar::sharded<ShardStore>& store) : port_(port), store_(store) {}

void TcpServer::start() {
    seastar::listen_options lo;
    lo.reuse_address = true;
    listener_ = seastar::listen(seastar::make_ipv4_address({port_}), lo);
    accept_loop_done_ = accept_loop();
}

seastar::future<> TcpServer::accept_loop() {
    return seastar::keep_doing([this] {
        return listener_.accept().then([this](seastar::accept_result ar) {
            applog.info("accepted connection from {} on shard {}", ar.remote_address,
                        seastar::this_shard_id());
            auto conn = seastar::make_lw_shared<Connection>(std::move(ar.connection), store_);
            (void)conn->handle().handle_exception([conn](std::exception_ptr e) {
                applog.warn("connection terminated with exception: {}", e);
            });
        });
    });
}

seastar::future<> TcpServer::stop() {
    listener_.abort_accept();
    return std::move(accept_loop_done_).handle_exception([](std::exception_ptr e) {
        applog.warn("accept loop stopped with exception: {}", e);
    });
}

}  // namespace mini_dragonfly
