// Phase 4: epoll event loop, keep-alive, idle timeouts, worker thread pool.

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "router.hpp"
#include "server.hpp"

static volatile std::sig_atomic_t g_running = 1;

static void on_signal(int) { g_running = 0; }

static void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // epoll_wait returns EINTR so the loop notices
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char** argv) {
    // Usage: httpserver [port] [static_dir] [-q]
    uint16_t port = 8080;
    std::string static_dir = "static";
    bool quiet = false;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-q")
            quiet = true;
        else if (positional == 0) {
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
    });
    router.add("POST", "/echo", [](const Request& req) {
        Response r;
        r.body = req.body;
        return r;
    });
    // Demo for concurrency: a slow handler must not block other clients.
    router.add("GET", "/slow", [](const Request&) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Response r;
        r.body = "finally\n";
        return r;
    });
    router.serve_static(static_dir);

    install_signal_handlers();

    // Handlers can block on disk or sleeps, so don't size the pool by cores alone.
    size_t workers = std::max<size_t>(4, std::thread::hardware_concurrency());

    Server server(port, router, workers, !quiet);
    std::printf("Listening on http://0.0.0.0:%u, serving ./%s, %zu workers (Ctrl+C to stop)\n",
                port, static_dir.c_str(), workers);

    server.run(g_running);

    std::puts("\nShutting down.");
    return 0;
}
