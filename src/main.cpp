#include <chrono>
#include <exception>
#include <seastar/core/app-template.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/closeable.hh>
#include <seastar/util/log.hh>

#include "expiry_sweeper.hh"
#include "server.hh"
#include "stop_signal.hh"
#include "store.hh"

namespace {

seastar::logger applog("main");
constexpr uint16_t kDefaultPort = 6380;
constexpr auto kExpirySweepInterval = std::chrono::milliseconds(100);

}  // namespace

int main(int argc, char** argv) {
    seastar::app_template app;
    app.add_options()("port",
                      boost::program_options::value<uint16_t>()->default_value(kDefaultPort),
                      "TCP port to listen on");

    return app.run(argc, argv, [&app] {
        return seastar::async([&app] {
            const auto port = app.configuration()["port"].as<uint16_t>();

            mini_dragonfly::stop_signal stop_signal;

            seastar::sharded<mini_dragonfly::ShardStore> store;
            store.start().get();
            auto stop_store = seastar::deferred_stop(store);

            // Declared (and so, on the way out, stopped) after store but
            // before server: shutdown order is server, then sweeper, then
            // store -- stopping the connection-handling and the periodic
            // sweep before the store they both call into is torn down.
            mini_dragonfly::ExpirySweeper sweeper(store, kExpirySweepInterval);
            sweeper.start();
            auto stop_sweeper = seastar::deferred_stop(sweeper);

            seastar::sharded<mini_dragonfly::TcpServer> server;
            server.start(port, std::ref(store)).get();
            auto stop_server = seastar::deferred_stop(server);

            // invoke_on_all's start() calls seastar::listen() on every
            // shard; if the port is already taken, that throws. Without
            // this, the exception unwound straight out of this seastar
            // thread and left the process stuck rather than exiting --
            // the deferred_stop guards above still run during that
            // unwinding, but nothing turned the failure into a visible
            // log line and a non-zero exit.
            try {
                server.invoke_on_all(&mini_dragonfly::TcpServer::start).get();
            } catch (...) {
                applog.error("failed to listen on port {}: {}", port, std::current_exception());
                return 1;
            }
            applog.info("listening on port {} across {} shard(s)", port, seastar::smp::count);

            stop_signal.wait().get();
            applog.info("shutting down");
            return 0;
        });
    });
}
