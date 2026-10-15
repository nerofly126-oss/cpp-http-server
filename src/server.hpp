#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "conn_limiter.hpp"
#include "fd.hpp"
#include "http.hpp"
#include "router.hpp"
#include "thread_pool.hpp"

// One epoll event loop. The program runs several, one per core. Each has its
// own listening socket bound with SO_REUSEPORT, so the kernel spreads new
// connections across the loops, and each owns its connections outright: no
// other thread ever touches one. Cheap handlers run right on the loop; blocking
// ones go to a worker pool shared by all loops, and the result comes back
// through the originating loop's own eventfd.
class Server {
public:
    Server(uint16_t port, const Router& router, ThreadPool& pool, ConnLimiter& limiter,
           bool log_requests);

    // Runs the loop until `running` becomes false.
    void run(const std::atomic<bool>& running);

    // Makes run() notice a change to `running` right away. Safe from any thread.
    void wake();

    // Only meaningful after run() has returned.
    uint64_t accepted() const { return accepted_; }
    uint64_t requests() const { return requests_; }

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
        uint32_t ip = 0;      // peer IPv4 address, for the connection limits
        Clock::time_point last_active;
        Clock::time_point request_start;  // when the current request's first byte arrived
        bool has_out() const { return out_off < out.size(); }
    };

    struct Done {
        uint64_t id;
        std::string bytes;
        bool keep_alive;
    };

    enum class WriteResult { Done, Blocked, Closed };

    void accept_all();
    void on_readable(Conn& c);
    void process_input(Conn& c);
    WriteResult write_out(Conn& c);
    bool flush(Conn& c);  // false if the connection was closed
    void respond_and_close(Conn& c, int status);
    void drain_completions();
    void expire_idle();
    void update_events(Conn& c);
    void close_conn(uint64_t id);
    void submit(uint64_t id, Request req);
    void log_request(const Request& req, int status) const;

    const Router& router_;
    ThreadPool& pool_;
    ConnLimiter& limiter_;
    bool log_requests_;
    Fd ep_;
    Fd wake_;
    Fd listener_;
    std::unordered_map<uint64_t, std::unique_ptr<Conn>> conns_;
    uint64_t next_id_ = 2;  // 0 = listener, 1 = wake eventfd
    Clock::time_point last_sweep_ = Clock::now();
    uint64_t accepted_ = 0;
    uint64_t requests_ = 0;
    std::mutex done_mu_;
    std::vector<Done> done_;
};
