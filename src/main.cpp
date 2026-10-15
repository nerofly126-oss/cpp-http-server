// Phase 7: several epoll event loops (one per core) sharing one worker pool.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "conn_limiter.hpp"
#include "fd.hpp"
#include "router.hpp"
#include "server.hpp"
#include "thread_pool.hpp"

constexpr size_t kMaxConnections = 10000;
constexpr size_t kMaxConnsPerIp = 256;

// SO_REUSEPORT lets two copies of this server share a port without any error,
// and then split the traffic between them. That is a nasty surprise (an old
// instance still running while you benchmark a new one), so refuse to start if
// anything already answers on the port.
static void ensure_port_is_free(uint16_t port) {
    Fd probe(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (probe && ::connect(probe.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
        std::fprintf(stderr, "port %u is already in use by another server\n", port);
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    // Block SIGINT and SIGTERM before any thread exists, so every thread inherits
    // the mask and only the sigwait() in main ever receives them.
    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // Usage: httpserver [port] [static_dir] [-q] [-l loops] [-w workers]
    uint16_t port = 8080;
    std::string static_dir = "static";
    bool quiet = false;
    size_t loops = std::max<size_t>(1, std::thread::hardware_concurrency());
    // Handlers can block on disk or sleeps, so don't size the pool by cores alone.
    size_t workers = std::max<size_t>(4, std::thread::hardware_concurrency());
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-q") {
            quiet = true;
        } else if ((arg == "-l" || arg == "-w") && i + 1 < argc) {
            size_t value = std::strtoul(argv[++i], nullptr, 10);
            (arg == "-l" ? loops : workers) = std::clamp<size_t>(value, 1, 64);
        } else if (positional == 0) {
            port = static_cast<uint16_t>(std::atoi(argv[i]));
            ++positional;
        } else if (positional == 1) {
            static_dir = arg;
            ++positional;
        }
    }

    Router router;
    router.add("GET", "/health", [](const Request&) {
        Response r;
        r.body = "ok\n";
        return r;
    }, RunOn::Loop);
    router.add("POST", "/echo", [](const Request& req) {
        Response r;
        r.body = req.body;
        return r;
    }, RunOn::Loop);
    // Demo for concurrency: a slow handler must not block other clients.
    router.add("GET", "/slow", [](const Request&) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Response r;
        r.body = "finally\n";
        return r;
    });
    router.serve_static(static_dir);

    ensure_port_is_free(port);

    ThreadPool pool(workers);
    ConnLimiter limiter(kMaxConnections, kMaxConnsPerIp);
    std::atomic<bool> running{true};

    std::vector<std::unique_ptr<Server>> servers;
    for (size_t i = 0; i < loops; ++i)
        servers.push_back(std::make_unique<Server>(port, router, pool, limiter, !quiet));

    std::vector<std::thread> threads;
    for (auto& server : servers) {
        Server* s = server.get();
        threads.emplace_back([s, &running] { s->run(running); });
    }

    std::printf("Listening on http://0.0.0.0:%u, serving ./%s, %zu event loops, %zu workers (Ctrl+C to stop)\n",
                port, static_dir.c_str(), loops, workers);

    int sig = 0;
    sigwait(&shutdown_signals, &sig);

    running = false;
    for (auto& server : servers) server->wake();
    for (auto& t : threads) t.join();
    pool.stop();  // after the loops, before the servers are destroyed

    std::puts("\nShutting down.");
    if (!quiet) {
        for (size_t i = 0; i < servers.size(); ++i)
            std::printf("  loop %zu: %llu connections, %llu requests\n", i,
                        static_cast<unsigned long long>(servers[i]->accepted()),
                        static_cast<unsigned long long>(servers[i]->requests()));
    }
    return 0;
}
