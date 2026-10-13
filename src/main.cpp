// Phase 3: router with handlers, static file serving, path traversal protection.
// Still blocking and one connection at a time. Concurrency is Phase 4.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "fd.hpp"
#include "http.hpp"
#include "router.hpp"

constexpr int kRecvTimeoutSec = 5;

static volatile std::sig_atomic_t g_running = 1;

static void on_sigint(int) { g_running = 0; }

static void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: accept() must return EINTR
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

static Fd make_listener(uint16_t port) {
    Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd) {
        std::perror("socket");
        std::exit(1);
    }

    int yes = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        std::perror("bind");
        std::exit(1);
    }
    if (::listen(fd.get(), SOMAXCONN) < 0) {
        std::perror("listen");
        std::exit(1);
    }
    return fd;
}

static void handle_client(Fd client, const Router& router) {
    // Without a timeout, one silent client freezes this whole server.
    timeval tv{};
    tv.tv_sec = kRecvTimeoutSec;
    setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    Request req;
    int rc = read_request(client.get(), req);
    if (rc == -1) return;  // client vanished, nothing to say

    Response res = (rc == 0) ? router.dispatch(req) : make_error(rc);

    std::printf("%s %s -> %d\n", req.method.c_str(), req.target.c_str(), res.status);
    send_all(client.get(), serialize(res));
}

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 8080;
    std::string static_dir = argc > 2 ? argv[2] : "static";

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
    router.serve_static(static_dir);

    install_signal_handlers();
    Fd listener = make_listener(port);
    std::printf("Listening on http://0.0.0.0:%u, serving ./%s (Ctrl+C to stop)\n", port,
                static_dir.c_str());

    while (g_running) {
        Fd client(::accept(listener.get(), nullptr, nullptr));
        if (!client) {
            if (errno == EINTR) continue;
            std::perror("accept");
            continue;
        }
        handle_client(std::move(client), router);
    }

    std::puts("\nShutting down.");
    return 0;
}
