#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// Connection limits shared by every event loop. The kernel spreads one client's
// connections across all loops, so a per-loop count would multiply the cap.
// The mutex is only taken when a connection opens or closes, never per request.
class ConnLimiter {
public:
    ConnLimiter(size_t max_total, size_t max_per_ip) : max_total_(max_total), max_per_ip_(max_per_ip) {}

    // Claims a slot for this peer address. Every success needs one release().
    bool try_acquire(uint32_t ip) {
        std::lock_guard<std::mutex> lock(mu_);
        if (total_ >= max_total_) return false;
        auto it = per_ip_.find(ip);
        if (it != per_ip_.end() && it->second >= max_per_ip_) return false;
        ++per_ip_[ip];
        ++total_;
        return true;
    }

    void release(uint32_t ip) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = per_ip_.find(ip);
        if (it == per_ip_.end()) return;
        if (--it->second == 0) per_ip_.erase(it);
        --total_;
    }

    size_t total() const {
        std::lock_guard<std::mutex> lock(mu_);
        return total_;
    }

private:
    const size_t max_total_;
    const size_t max_per_ip_;
    mutable std::mutex mu_;
    size_t total_ = 0;
    std::unordered_map<uint32_t, size_t> per_ip_;
};
