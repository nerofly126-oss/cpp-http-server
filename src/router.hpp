#pragma once
#include <functional>
#include <map>
#include <string>
#include <unordered_map>

#include "http.hpp"

using Handler = std::function<Response(const Request&)>;

class Router {
public:
    // Register a handler for an exact path and method.
    void add(const std::string& method, const std::string& path, Handler handler);

    // Serve files from root_dir for any GET/HEAD path no handler claims.
    void serve_static(const std::string& root_dir) { static_root_ = root_dir; }

    Response dispatch(const Request& req) const;

private:
    Response serve_file(const std::string& url_path) const;

    std::unordered_map<std::string, std::map<std::string, Handler>> routes_;  // path -> method
    std::string static_root_;
};
