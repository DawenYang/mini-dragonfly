#include "server.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
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
#include <utility>
#include <variant>
#include <vector>

#include "commands.hh"
#include "resp.h"

namespace mini_dragonfly {

namespace {

seastar::logger applog("server");

constexpr auto kShutdownGracePeriod = std::chrono::seconds(2);
constexpr auto kShutdownPollInterval = std::chrono::milliseconds(20);
constexpr auto kAcceptErrorBackoff = std::chrono::milliseconds(100);

// Which shard owns a given key. Lives here (not in ShardStore) since it's
// routing logic, not storage logic -- keeps ShardStore itself free of any
// Seastar dependency so it can be unit-tested without a reactor.
unsigned owner_shard(const std::string& key) {
    return static_cast<unsigned>(std::hash<std::string>{}(key) % seastar::smp::count);
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
            // Unbounded growth point -- see buf_'s own declaration below.
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
    // below) and reply on out_. The table itself, and everything about
    // each command except how to route to a shard, lives in commands.hh --
    // this class only parses, looks the spec up, and executes it.
    seastar::future<> dispatch(std::vector<std::string> args) {
        const auto cmd = commands::to_upper(args[0]);
        const auto& table = commands::table();
        const auto it = table.find(cmd);
        if (it == table.end() || !commands::arity_matches(it->second.arity, args.size())) {
            return out_.write("-ERR unknown command or wrong number of arguments\r\n");
        }
        return execute(it->second, std::move(args));
    }

    // Resolves this command's key(s) from spec.first_key/last_key/key_step,
    // routes to the owning shard(s) via Seastar's own cross-shard messaging
    // (sharded<T>::invoke_on, backed by smp_message_queue -- if a key's
    // shard happens to be this one, Seastar calls through directly without
    // going through the queue at all), and writes the reply. Does not
    // flush -- parse_next_command() does that once, right before it would
    // otherwise block waiting for more input, so a pipelined batch of
    // commands costs one flush instead of one per command; handle()'s
    // cleanup closing out_ covers the final reply too.
    //
    // A command with no keys at all (spec.first_key == 0) still goes
    // through here, just routed to whichever shard is already running it
    // -- so there's exactly one path below for "no keys", "one key", and
    // "many keys", rather than a separate one per shape.
    seastar::future<> execute(const commands::CommandSpec& spec, std::vector<std::string> args) {
        if (const auto* handler = std::get_if<commands::Handler>(&spec.handler)) {
            const unsigned shard = spec.first_key == 0
                                       ? seastar::this_shard_id()
                                       : owner_shard(args[static_cast<size_t>(spec.first_key)]);
            const commands::Handler h = *handler;
            return store_
                .invoke_on(shard, [args = std::move(args),
                                   h](ShardStore& s) mutable { return h(s, args); })
                .then([this](std::string reply) { return out_.write(std::move(reply)); });
        }

        const auto multi_handler = std::get<commands::MultiKeyHandler>(spec.handler);
        const auto first = static_cast<size_t>(spec.first_key);
        const auto last =
            spec.last_key == -1 ? args.size() - 1 : static_cast<size_t>(spec.last_key);
        const auto step = static_cast<size_t>(spec.key_step);

        std::vector<seastar::future<bool>> futures;
        futures.reserve((last - first) / step + 1);
        for (size_t i = first; i <= last; i += step) {
            std::string key = std::move(args[i]);
            const unsigned shard = owner_shard(key);
            futures.push_back(
                store_.invoke_on(shard, [key = std::move(key), multi_handler](ShardStore& s) {
                    return multi_handler(s, key);
                }));
        }
        return seastar::when_all_succeed(std::move(futures))
            .then([this](std::vector<bool> results) {
                const long long count = std::count(results.begin(), results.end(), true);
                return out_.write(resp::encode_integer(count));
            });
    }

    seastar::connected_socket socket_;
    seastar::input_stream<char> in_;
    seastar::output_stream<char> out_;
    seastar::sharded<ShardStore>& store_;
    // Unbounded: a client can declare a single bulk argument up to
    // resp.cpp's kMaxBulkLen (512 MB), and this grows to hold it while the
    // rest of that argument's bytes trickle in -- there's no cap here tied
    // to connection count or total server memory, so a client willing to
    // actually send that much data can make this connection alone hold
    // that much. Known limitation, acceptable for this project's current
    // scope; a deployment facing untrusted clients would want a
    // per-connection (and/or global) memory budget enforced here,
    // independent of resp.cpp's per-field limits.
    std::string buf_;
    // How many bytes at the front of buf_ have already been parsed into
    // commands but not yet physically erased -- see read_more_or_done().
    size_t buf_pos_ = 0;
    bool stop_ = false;
};

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
