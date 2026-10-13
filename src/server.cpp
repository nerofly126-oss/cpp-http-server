#include "server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint64_t kListenerId = 0;
constexpr uint64_t kWakeId = 1;
constexpr int kMaxEvents = 128;
constexpr int kIdleTimeoutSec = 10;      // no bytes at all for this long: drop the connection
constexpr int kRequestTimeoutSec = 15;   // a request must finish arriving within this, however slowly it drips
constexpr size_t kMaxConnsPerIp = 256;
constexpr size_t kMaxConnections = 10000;
constexpr size_t kMaxBufferedInput = kMaxHeaderBytes + kMaxBodyBytes + 4;

Fd make_listener(uint16_t port) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
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

void epoll_add(int ep, int fd, uint64_t id, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = id;
    if (::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
        std::perror("epoll_ctl add");
        std::exit(1);
    }
}

}  // namespace

// ------------------------------------------------------------------- setup

Server::Server(uint16_t port, const Router& router, size_t workers, bool log_requests)
    : router_(router),
      log_requests_(log_requests),
      ep_(::epoll_create1(EPOLL_CLOEXEC)),
      wake_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      listener_(make_listener(port)),
      pool_(workers) {
    if (!ep_ || !wake_) {
        std::perror("epoll/eventfd");
        std::exit(1);
    }
    epoll_add(ep_.get(), listener_.get(), kListenerId, EPOLLIN);
    epoll_add(ep_.get(), wake_.get(), kWakeId, EPOLLIN);
}

// -------------------------------------------------------------- event loop

void Server::run(volatile std::sig_atomic_t& running) {
    epoll_event events[kMaxEvents];

    while (running) {
        int n = ::epoll_wait(ep_.get(), events, kMaxEvents, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;  // a signal arrived, re-check `running`
            std::perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            const uint64_t id = events[i].data.u64;
            const uint32_t ev = events[i].events;

            if (id == kListenerId) {
                accept_all();
            } else if (id == kWakeId) {
                drain_completions();
            } else {
                // Look up by id, never by pointer: the connection may have been
                // closed earlier in this same batch of events.
                auto it = conns_.find(id);
                if (it == conns_.end()) continue;
                Conn& c = *it->second;

                if (ev & (EPOLLERR | EPOLLHUP)) {
                    close_conn(id);
                    continue;
                }
                if ((ev & EPOLLOUT) && !flush(c)) continue;
                if (ev & EPOLLIN) on_readable(c);
            }
        }
        expire_idle();
    }
}

void Server::accept_all() {
    for (;;) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof peer;
        int fd = ::accept4(listener_.get(), reinterpret_cast<sockaddr*>(&peer), &peer_len,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            std::perror("accept4");  // e.g. EMFILE: raise `ulimit -n` for big load tests
            return;
        }
        const uint32_t ip = peer.sin_addr.s_addr;
        if (conns_.size() >= kMaxConnections || per_ip_[ip] >= kMaxConnsPerIp) {
            ::close(fd);  // one client must not be able to eat every slot
            continue;
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        auto c = std::make_unique<Conn>();
        c->id = next_id_++;
        c->fd = Fd(fd);
        c->last_active = Clock::now();
        c->events = EPOLLIN;
        c->ip = ip;

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u64 = c->id;
        if (::epoll_ctl(ep_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
            std::perror("epoll_ctl add");
            continue;  // c goes out of scope and closes the fd
        }
        ++per_ip_[ip];
        conns_.emplace(c->id, std::move(c));
    }
}

void Server::on_readable(Conn& c) {
    char buf[16 * 1024];
    ssize_t n = ::recv(c.fd.get(), buf, sizeof buf, 0);

    if (n > 0) {
        const auto now = Clock::now();
        c.last_active = now;
        if (c.in.empty()) c.request_start = now;  // first byte of a new request
        c.in.append(buf, static_cast<size_t>(n));
        if (c.in.size() > kMaxBufferedInput) {  // pipelining flood
            close_conn(c.id);
            return;
        }
        process_input(c);
        return;
    }
    if (n == 0) {
        if (c.busy || c.has_out()) {
            // Client half-closed but still deserves its response. Stop
            // listening for reads, or level-triggered epoll spins on EOF.
            c.peer_closed = true;
            c.close_after_write = true;
            update_events(c);
        } else {
            close_conn(c.id);
        }
        return;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
    close_conn(c.id);
}

// Parses at most one request and hands it to the pool. Called whenever new
// bytes arrive or the previous response has been fully written.
void Server::process_input(Conn& c) {
    if (c.busy || c.has_out() || c.close_after_write || c.in.empty()) return;

    Request req;
    ParseResult pr = parse_request(c.in, req);
    switch (pr.status) {
        case ParseStatus::Incomplete:
            return;
        case ParseStatus::Error:
            respond_and_close(c, pr.error);
            return;
        case ParseStatus::Complete:
            c.in.erase(0, pr.consumed);
            if (!c.in.empty()) c.request_start = Clock::now();  // pipelined bytes already waiting
            c.busy = true;
            submit(c.id, std::move(req));
            return;
    }
}

bool Server::flush(Conn& c) {
    const uint64_t id = c.id;

    while (c.has_out()) {
        ssize_t n = ::send(c.fd.get(), c.out.data() + c.out_off, c.out.size() - c.out_off,
                           MSG_NOSIGNAL);
        if (n > 0) {
            c.out_off += static_cast<size_t>(n);
            c.last_active = Clock::now();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            update_events(c);  // socket buffer full: wait for EPOLLOUT
            return true;
        }
        close_conn(id);
        return false;
    }

    c.out.clear();
    c.out_off = 0;
    if (c.close_after_write) {
        close_conn(id);
        return false;
    }
    update_events(c);
    process_input(c);  // a pipelined request may already be waiting
    return conns_.count(id) != 0;
}

void Server::respond_and_close(Conn& c, int status) {
    if (log_requests_) std::printf("-> %d (closing)\n", status);
    c.out = serialize(make_error(status), false);
    c.out_off = 0;
    c.close_after_write = true;
    flush(c);
}

// ------------------------------------------------------------ worker handoff

void Server::submit(uint64_t id, Request req) {
    pool_.submit([this, id, req = std::move(req)]() {
        Response res;
        try {
            res = router_.dispatch(req);
        } catch (...) {
            res = make_error(500);
        }
        if (log_requests_)
            std::printf("%s %s -> %d\n", req.method.c_str(), req.target.c_str(), res.status);

        Done d{id, serialize(res, req.keep_alive), req.keep_alive};
        {
            std::lock_guard<std::mutex> lock(done_mu_);
            done_.push_back(std::move(d));
        }
        uint64_t one = 1;
        ssize_t n = ::write(wake_.get(), &one, sizeof one);  // wake the event loop
        (void)n;
    });
}

void Server::drain_completions() {
    uint64_t counter;
    ssize_t n = ::read(wake_.get(), &counter, sizeof counter);
    (void)n;

    std::vector<Done> batch;
    {
        std::lock_guard<std::mutex> lock(done_mu_);
        batch.swap(done_);
    }

    for (auto& d : batch) {
        auto it = conns_.find(d.id);
        if (it == conns_.end()) continue;  // client left while the worker ran
        Conn& c = *it->second;
        c.busy = false;
        c.out = std::move(d.bytes);
        c.out_off = 0;
        if (!d.keep_alive) c.close_after_write = true;
        flush(c);
    }
}

// ---------------------------------------------------------------- housekeeping

void Server::update_events(Conn& c) {
    uint32_t want = (c.peer_closed ? 0u : static_cast<uint32_t>(EPOLLIN)) |
                    (c.has_out() ? static_cast<uint32_t>(EPOLLOUT) : 0u);
    if (want == c.events) return;
    epoll_event ev{};
    ev.events = want;
    ev.data.u64 = c.id;
    ::epoll_ctl(ep_.get(), EPOLL_CTL_MOD, c.fd.get(), &ev);
    c.events = want;
}

void Server::close_conn(uint64_t id) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return;
    auto ip = per_ip_.find(it->second->ip);
    if (ip != per_ip_.end() && --ip->second == 0) per_ip_.erase(ip);
    conns_.erase(it);  // Fd destructor closes the socket and removes it from epoll
}

void Server::expire_idle() {
    const auto now = Clock::now();
    if (now - last_sweep_ < std::chrono::seconds(1)) return;
    last_sweep_ = now;

    std::vector<uint64_t> expired;
    for (const auto& [id, c] : conns_) {
        if (c->busy) continue;  // a worker is on it, not the client's fault
        const bool idle = now - c->last_active > std::chrono::seconds(kIdleTimeoutSec);
        // Slowloris: dripping one byte at a time keeps last_active fresh, so the
        // request needs its own deadline measured from its first byte.
        const bool too_slow = !c->in.empty() && !c->has_out() &&
                              now - c->request_start > std::chrono::seconds(kRequestTimeoutSec);
        if (idle || too_slow) expired.push_back(id);
    }
    for (uint64_t id : expired) {
        auto it = conns_.find(id);
        if (it == conns_.end()) continue;
        Conn& c = *it->second;
        if (!c.in.empty() && !c.has_out())
            respond_and_close(c, 408);  // stalled mid-request
        else
            close_conn(id);  // idle keep-alive or stuck writer
    }
}
