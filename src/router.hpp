#pragma once
#include <functional>
#include <map>
#include <string>
#include <unordered_map>

#include "http.hpp"

using Handler = std::function<Response(const Request&)>;

// Where a handler runs. Loop: right on the event loop thread, for handlers that
// are cheap and never block. Worker: on the shared pool, for anything that may
// sleep, wait on disk, or take long.
enum class RunOn { Worker, Loop };

class Router {
public:
    // Register a handler for an exact path and method.
    void add(const std::string& method, const std::string& path, Handler handler,
             RunOn where = RunOn::Worker);

    // True if dispatch(req) is cheap enough to run on the event loop thread:
    // a registered RunOn::Loop handler, or a cheap error (405, bad escape).
    // Static files and unknown paths may touch the disk, so they say false.
    bool runs_on_loop(const Request& req) const;

    // Serve files from root_dir for any GET/HEAD path no handler claims.
    void serve_static(const std::string& root_dir) { static_root_ = root_dir; }

    Response dispatch(const Request& req) const;

private:
    Response serve_file(const std::string& url_path) const;

    struct Route {
        Handler handler;
        RunOn where;
    };

    std::unordered_map<std::string, std::map<std::string, Route>> routes_;  // path -> method
    std::string static_root_;
};
