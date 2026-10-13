#pragma once
#include <chrono>
#include <csignal>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fd.hpp"
#include "http.hpp"
#include "router.hpp"
#include "thread_pool.hpp"

// One epoll event loop owns every socket. Complete requests are handed to a
// worker pool; workers send finished responses back through an eventfd.
class Server {
public:
    Server(uint16_t port, const Router& router, size_t workers, bool log_requests);

    // Runs the loop until `running` becomes 0 (cleared by a signal handler).
    void run(volatile std::sig_atomic_t& running);

private:
    using Clock = std::chrono::steady_clock;

    struct Conn {
        uint64_t id = 0;
        Fd fd;
        std::string in;   // bytes received but not yet parsed
        std::string out;  // response bytes waiting to be sent
        size_t out_off = 0;
        bool busy = false;  // a worker is handling this connection's request
        bool close_after_write = false;
        bool peer_closed = false;
        uint32_t events = 0;  // what we currently ask epoll for
        uint32_t ip = 0;      // peer IPv4 address, for the per-IP connection cap
        Clock::time_point last_active;
        Clock::time_point request_start;  // when the current request's first byte arrived
        bool has_out() const { return out_off < out.size(); }
    };

    struct Done {
        uint64_t id;
        std::string bytes;
        bool keep_alive;
    };

    void accept_all();
    void on_readable(Conn& c);
    void process_input(Conn& c);
    bool flush(Conn& c);  // false if the connection was closed
    void respond_and_close(Conn& c, int status);
    void drain_completions();
    void expire_idle();
    void update_events(Conn& c);
    void close_conn(uint64_t id);
    void submit(uint64_t id, Request req);

    // Declaration order matters: pool_ is last so it is destroyed first,
    // while the queue and eventfd its workers use are still alive.
    const Router& router_;
    bool log_requests_;
    Fd ep_;
    Fd wake_;
    Fd listener_;
    std::unordered_map<uint64_t, std::unique_ptr<Conn>> conns_;
    std::unordered_map<uint32_t, size_t> per_ip_;  // open connections per peer address
    uint64_t next_id_ = 2;  // 0 = listener, 1 = wake eventfd
    Clock::time_point last_sweep_ = Clock::now();
    std::mutex done_mu_;
    std::vector<Done> done_;
    ThreadPool pool_;
};
