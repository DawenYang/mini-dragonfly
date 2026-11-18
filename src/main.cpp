#include "server.hh"
#include "stop_signal.hh"
#include "store.hh"

#include <seastar/core/app-template.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/log.hh>

namespace {

seastar::logger applog("main");
constexpr uint16_t kDefaultPort = 6380;

}  // namespace

int main(int argc, char** argv) {
    seastar::app_template app;
    app.add_options()("port", boost::program_options::value<uint16_t>()->default_value(kDefaultPort),
                       "TCP port to listen on");

    return app.run(argc, argv, [&app] {
        return seastar::async([&app] {
            const auto port = app.configuration()["port"].as<uint16_t>();

            mini_dragonfly::stop_signal stop_signal;

            seastar::sharded<mini_dragonfly::ShardStore> store;
            store.start().get();

            seastar::sharded<mini_dragonfly::TcpServer> server;
            server.start(port, std::ref(store)).get();
            server.invoke_on_all(&mini_dragonfly::TcpServer::start).get();
            applog.info("listening on port {} across {} shard(s)", port, seastar::smp::count);

            stop_signal.wait().get();

            applog.info("shutting down");
            server.stop().get();
            store.stop().get();
        });
    });
}
