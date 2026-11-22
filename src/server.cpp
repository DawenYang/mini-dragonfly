#include "server.hh"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <seastar/core/loop.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/shard_id.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/log.hh>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "resp.h"

namespace mini_dragonfly {

namespace {

seastar::logger applog("server");

constexpr auto kShutdownGracePeriod = std::chrono::seconds(2);
constexpr auto kShutdownPollInterval = std::chrono::milliseconds(20);
constexpr auto kAcceptErrorBackoff = std::chrono::milliseconds(100);

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

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

}  // namespace

namespace detail {

class Connection {
public:
    Connection(seastar::connected_socket socket, seastar::sharded<ShardStore>& store)
        : socket_(std::move(socket)), in_(socket_.input()), out_(socket_.output()), store_(store) {}

    seastar::future<> handle() {
        return seastar::do_until([this] { return stop_; }, [this] { return process_one(); })
            .finally([this] { return in_.close().finally([this] { return out_.close(); }); });
    }

    // Aborts any read currently blocked waiting for more data from this
    // client, so a shutdown doesn't leave the connection's fiber running
    // (and reading from a store that's about to be torn down) forever.
    void shutdown_input() { socket_.shutdown_input(); }

    // Used only once a shutdown's grace period has elapsed and this
    // connection is still around -- shutdown_input() alone can't unblock a
    // fiber that's stuck writing to a client that's stopped reading, so
    // force both directions closed instead of waiting on it forever.
    void force_close() {
        socket_.shutdown_input();
        socket_.shutdown_output();
    }

private:
    // Tries to parse a command out of whatever's already buffered. If
    // that's not enough, flushes any replies written so far -- since
    // we're about to block, a pipelined batch gets exactly one flush here
    // rather than one per command -- then reads more and retries.
    // Resolves with a ParseResult whose status is Ok or Error once a
    // definitive answer is known, or Incomplete if eof was reached while a
    // command was still incomplete.
    seastar::future<resp::ParseResult> parse_next_command() {
        auto result = resp::parse_command(std::string_view(buf_).substr(buf_pos_));
        if (result.status != resp::Status::Incomplete) {
            return seastar::make_ready_future<resp::ParseResult>(std::move(result));
        }
        return out_.flush().then(
            [this] { return seastar::repeat_until_value([this] { return read_more_or_done(); }); });
    }

    seastar::future<std::optional<resp::ParseResult>> read_more_or_done() {
        return in_.read().then([this](seastar::temporary_buffer<char> chunk) {
            if (chunk.empty()) {
                resp::ParseResult eof_result;
                eof_result.status = resp::Status::Incomplete;
                return std::optional<resp::ParseResult>(std::move(eof_result));
            }
            // Reclaim whatever's already been consumed before growing the
            // buffer, so a long-lived connection's buf_ doesn't creep up
            // in size forever. This is the only place buf_ ever shifts its
            // remaining bytes down -- at most once per actual read(), not
            // once per parsed command.
            if (buf_pos_ > 0) {
                buf_.erase(0, buf_pos_);
                buf_pos_ = 0;
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
                // No explicit flush -- handle()'s cleanup closes out_,
                // which flushes any unsent bytes (this reply included)
                // before the underlying socket actually closes.
                return out_.write("-ERR protocol error\r\n");
            }
            buf_pos_ += result.consumed;
            if (buf_pos_ == buf_.size()) {
                // Nothing pipelined behind this command -- reclaim the
                // whole buffer right away instead of leaving it to dangle
                // until the next read() needs to compact it.
                buf_.clear();
                buf_pos_ = 0;
            }
            if (result.args.empty()) {
                // "*0\r\n": a valid empty command. Nothing to execute or reply to.
                return seastar::make_ready_future<>();
            }
            return dispatch(std::move(result.args));
        });
    }

    // Every command handler takes the full command (args[0] is the command
    // name itself, already arity-checked by dispatch() against the table
    // below) and replies on out_. Arity is "total argument count", so e.g.
    // SET key value has min_args == max_args == 3.
    using Handler = seastar::future<> (Connection::*)(std::vector<std::string>);
    struct CommandSpec {
        int min_args;
        int max_args;  // -1 means unlimited.
        Handler handler;
    };
    static const std::unordered_map<std::string, CommandSpec>& command_table();

    seastar::future<> dispatch(std::vector<std::string> args) {
        const auto cmd = to_upper(args[0]);
        const auto& table = command_table();
        const auto it = table.find(cmd);
        const int n = static_cast<int>(args.size());
        if (it == table.end() || n < it->second.min_args ||
            (it->second.max_args != -1 && n > it->second.max_args)) {
            return out_.write("-ERR unknown command or wrong number of arguments\r\n");
        }
        return (this->*it->second.handler)(std::move(args));
    }

    // None of these flush -- parse_next_command() does that once, right
    // before it would otherwise block waiting for more input, so a
    // pipelined batch of commands costs one flush instead of one per
    // command. handle()'s cleanup closing out_ covers the final reply too.

    // Doesn't touch the store or go cross-shard -- answered directly on
    // whichever shard accepted the connection.
    seastar::future<> do_ping(std::vector<std::string> args) {
        const std::string reply =
            args.size() == 2 ? resp::encode_bulk(std::optional(std::move(args[1]))) : "+PONG\r\n";
        return out_.write(reply);
    }

    seastar::future<> do_echo(std::vector<std::string> args) {
        return out_.write(resp::encode_bulk(std::optional(std::move(args[1]))));
    }

    // Routes to whichever shard owns this key via Seastar's own cross-shard
    // messaging (sharded<T>::invoke_on, backed by smp_message_queue). If
    // that happens to be this shard, Seastar calls through directly without
    // going through the queue at all.
    seastar::future<> do_get(std::vector<std::string> args) {
        std::string& key = args[1];
        const unsigned shard = owner_shard(key);
        return store_.invoke_on(shard, [key = std::move(key)](ShardStore& s) { return s.get(key); })
            .then([this](std::optional<std::string> value) {
                return out_.write(resp::encode_bulk(value));
            });
    }

    seastar::future<> do_set(std::vector<std::string> args) {
        std::string& key = args[1];
        std::string& value = args[2];
        const unsigned shard = owner_shard(key);
        return store_
            .invoke_on(shard, [key = std::move(key), value = std::move(value)](
                                  ShardStore& s) { s.set(std::move(key), std::move(value)); })
            .then([this] { return out_.write("+OK\r\n"); });
    }

    seastar::future<> do_expire(std::vector<std::string> args) {
        std::string& key = args[1];
        const std::string& seconds_str = args[2];
        long long seconds = 0;
        if (!parse_integer_arg(seconds_str, seconds)) {
            return out_.write("-ERR value is not an integer or out of range\r\n");
        }
        if (seconds < kMinExpirySeconds || seconds > kMaxExpirySeconds) {
            // ShardStore::expire() would clamp this safely rather than
            // overflow, but silently treating "expire in 10^18 seconds" as
            // "expire in ~292 years" without telling the caller is still a
            // surprise worth rejecting outright.
            return out_.write("-ERR invalid expire time\r\n");
        }
        const unsigned shard = owner_shard(key);
        return store_
            .invoke_on(shard, [key = std::move(key),
                               seconds](ShardStore& s) { return s.expire(key, seconds); })
            .then(
                [this](bool did_set) { return out_.write(resp::encode_integer(did_set ? 1 : 0)); });
    }

    // Shared by do_del/do_exists: route each key to its owning shard in
    // parallel, then count how many of them came back true.
    seastar::future<> do_count_keys(std::vector<std::string> keys,
                                    bool (ShardStore::*member)(const std::string&)) {
        std::vector<seastar::future<bool>> futures;
        futures.reserve(keys.size());
        for (auto& key : keys) {
            const unsigned shard = owner_shard(key);
            futures.push_back(store_.invoke_on(
                shard, [key = std::move(key), member](ShardStore& s) { return (s.*member)(key); }));
        }
        return seastar::when_all_succeed(std::move(futures))
            .then([this](std::vector<bool> results) {
                const long long count = std::count(results.begin(), results.end(), true);
                return out_.write(resp::encode_integer(count));
            });
    }

    seastar::future<> do_del(std::vector<std::string> args) {
        return do_count_keys(
            {std::make_move_iterator(args.begin() + 1), std::make_move_iterator(args.end())},
            &ShardStore::del);
    }

    seastar::future<> do_exists(std::vector<std::string> args) {
        return do_count_keys(
            {std::make_move_iterator(args.begin() + 1), std::make_move_iterator(args.end())},
            &ShardStore::exists);
    }

    seastar::future<> do_incr(std::vector<std::string> args) {
        std::string& key = args[1];
        const unsigned shard = owner_shard(key);
        return store_
            .invoke_on(shard, [key = std::move(key)](ShardStore& s) { return s.incr(key); })
            .then([this](ShardStore::IncrOutcome outcome) {
                if (outcome.status == ShardStore::IncrOutcome::Status::NotInteger) {
                    return out_.write("-ERR value is not an integer or out of range\r\n");
                }
                if (outcome.status == ShardStore::IncrOutcome::Status::Overflow) {
                    return out_.write("-ERR increment or decrement would overflow\r\n");
                }
                return out_.write(resp::encode_integer(outcome.value));
            });
    }

    // Stubs: enough for clients/tools that probe these on connect (e.g.
    // CLIENT SETINFO) or introspect the server (COMMAND, CONFIG GET) to
    // get a well-formed reply, without actually implementing any of it.
    seastar::future<> do_command(std::vector<std::string>) { return out_.write("*0\r\n"); }

    seastar::future<> do_config(std::vector<std::string> args) {
        if (args.size() >= 2 && to_upper(args[1]) == "GET") {
            return out_.write("*0\r\n");
        }
        return out_.write("+OK\r\n");
    }

    seastar::future<> do_client(std::vector<std::string>) { return out_.write("+OK\r\n"); }

    seastar::connected_socket socket_;
    seastar::input_stream<char> in_;
    seastar::output_stream<char> out_;
    seastar::sharded<ShardStore>& store_;
    std::string buf_;
    // How many bytes at the front of buf_ have already been parsed into
    // commands but not yet physically erased -- see read_more_or_done().
    size_t buf_pos_ = 0;
    bool stop_ = false;
};

// Defined out-of-line since it needs Connection to already be a complete
// type for the member-function pointers below.
const std::unordered_map<std::string, Connection::CommandSpec>& Connection::command_table() {
    static const std::unordered_map<std::string, CommandSpec> table = {
        {"PING", {1, 2, &Connection::do_ping}},        {"ECHO", {2, 2, &Connection::do_echo}},
        {"GET", {2, 2, &Connection::do_get}},          {"SET", {3, 3, &Connection::do_set}},
        {"EXPIRE", {3, 3, &Connection::do_expire}},    {"DEL", {2, -1, &Connection::do_del}},
        {"EXISTS", {2, -1, &Connection::do_exists}},   {"INCR", {2, 2, &Connection::do_incr}},
        {"COMMAND", {1, -1, &Connection::do_command}}, {"CONFIG", {1, -1, &Connection::do_config}},
        {"CLIENT", {1, -1, &Connection::do_client}},
    };
    return table;
}

}  // namespace detail

using detail::Connection;

TcpServer::TcpServer(uint16_t port, seastar::sharded<ShardStore>& store)
    : port_(port), store_(store) {}

// Declared out-of-line (rather than left implicit) so that TUs which only
// see the Connection forward declaration in server.hh -- e.g. main.cpp,
// where sharded<TcpServer> destroys per-shard instances -- don't need
// Connection to be a complete type.
TcpServer::~TcpServer() = default;

void TcpServer::start() {
    seastar::listen_options lo;
    lo.reuse_address = true;
    listener_ = seastar::listen(seastar::make_ipv4_address({port_}), lo);
    listening_ = true;
    accept_loop_done_ = accept_loop();
}

seastar::future<> TcpServer::accept_loop() {
    return seastar::keep_doing([this] {
        return listener_.accept()
            .then([this](seastar::accept_result ar) {
                applog.info("accepted connection from {} on shard {}", ar.remote_address,
                            seastar::this_shard_id());
                auto conn = seastar::make_lw_shared<Connection>(std::move(ar.connection), store_);
                connections_.insert(conn);
                connections_gate_.enter();
                (void)conn->handle()
                    .handle_exception([conn](std::exception_ptr e) {
                        applog.warn("connection terminated with exception: {}", e);
                    })
                    .finally([this, conn] {
                        connections_.erase(conn);
                        connections_gate_.leave();
                    });
            })
            .handle_exception([this](std::exception_ptr e) -> seastar::future<> {
                if (stopping_) {
                    // stop() just called abort_accept(), and this is that
                    // cancellation surfacing -- let it propagate so
                    // keep_doing actually stops instead of spinning
                    // forever retrying against an aborted listener.
                    return seastar::make_exception_future<>(e);
                }
                // A transient accept() failure (e.g. EMFILE/ENFILE when
                // the process is out of file descriptors) used to kill
                // keep_doing permanently, silently leaving this shard
                // unable to accept ever again. Log it and keep listening
                // instead, with a short backoff so a persistent failure
                // doesn't spin the loop at full CPU.
                applog.warn("accept() failed, will keep listening: {}", e);
                return seastar::sleep(kAcceptErrorBackoff);
            });
    });
}

seastar::future<> TcpServer::stop() {
    // listener_ is still default-constructed (no underlying impl) if
    // start() never got far enough to assign it -- e.g. seastar::listen()
    // threw because the port was already taken elsewhere. Calling
    // abort_accept() on that would dereference a null pointer.
    if (listening_) {
        stopping_ = true;
        listener_.abort_accept();
    }
    return std::move(accept_loop_done_)
        .handle_exception(
            [](std::exception_ptr e) { applog.warn("accept loop stopped with exception: {}", e); })
        .then([this] {
            // Unblock any connection currently blocked waiting for more
            // data from its client -- shutdown_input() makes a pending (or
            // future) read fail immediately instead of leaving the
            // connection's fiber running past shutdown, still able to call
            // into a store that's about to be torn down.
            for (auto& conn : connections_) {
                conn->shutdown_input();
            }
            const auto deadline = std::chrono::steady_clock::now() + kShutdownGracePeriod;
            return seastar::do_until(
                       [this, deadline] {
                           return connections_.empty() ||
                                  std::chrono::steady_clock::now() >= deadline;
                       },
                       [] { return seastar::sleep(kShutdownPollInterval); })
                .then([this] {
                    // shutdown_input() doesn't unblock a connection stuck
                    // writing to a client that's stopped reading -- after
                    // the grace period, force those closed outright rather
                    // than let connections_gate_.close() wait forever.
                    for (auto& conn : connections_) {
                        conn->force_close();
                    }
                    return connections_gate_.close();
                });
        });
}

}  // namespace mini_dragonfly
