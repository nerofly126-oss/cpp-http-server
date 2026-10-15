#include "router.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ------------------------------------------------------------------ helpers

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes %XX sequences. Returns false on malformed input.
static bool percent_decode(std::string_view in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) return false;
        int hi = hex_val(in[i + 1]);
        int lo = hex_val(in[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return true;
}

static std::string mime_type(const std::string& ext) {
    static const std::unordered_map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".txt", "text/plain; charset=utf-8"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".ico", "image/x-icon"},
    };
    auto it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

// ------------------------------------------------------------------- router

void Router::add(const std::string& method, const std::string& path, Handler handler,
                 RunOn where) {
    routes_[path][method] = Route{std::move(handler), where};
}

bool Router::runs_on_loop(const Request& req) const {
    std::string_view raw = req.target;
    raw = raw.substr(0, raw.find('?'));

    std::string path;
    if (!percent_decode(raw, path)) return true;  // 400 is cheap

    auto route = routes_.find(path);
    if (route == routes_.end()) return false;  // static file or 404: may touch the disk

    const auto& methods = route->second;
    auto it = methods.find(req.method);
    if (it == methods.end() && req.method == "HEAD") it = methods.find("GET");
    if (it == methods.end()) return true;  // 405 is cheap
    return it->second.where == RunOn::Loop;
}

Response Router::serve_file(const std::string& url_path) const {
    if (url_path.find('\0') != std::string::npos) return make_error(400);

    std::error_code ec;
    fs::path root = fs::canonical(static_root_, ec);
    if (ec) return make_error(404);

    // Resolve ".." and symlinks, then make sure we are still inside root.
    fs::path resolved = fs::weakly_canonical(root / url_path.substr(1), ec);
    if (ec) return make_error(404);
    fs::path rel = resolved.lexically_relative(root);
    if (rel.empty() || *rel.begin() == "..") return make_error(403);

    if (fs::is_directory(resolved, ec)) resolved /= "index.html";
    if (!fs::is_regular_file(resolved, ec)) return make_error(404);

    std::ifstream file(resolved, std::ios::binary);
    if (!file) return make_error(404);
    std::ostringstream ss;
    ss << file.rdbuf();

    Response r;
    r.content_type = mime_type(resolved.extension().string());
    r.body = ss.str();
    return r;
}

Response Router::dispatch(const Request& req) const {
    std::string_view raw = req.target;
    raw = raw.substr(0, raw.find('?'));  // query string ignored for now

    std::string path;
    if (!percent_decode(raw, path)) return make_error(400);

    const bool head = (req.method == "HEAD");

    auto route = routes_.find(path);
    if (route != routes_.end()) {
        const auto& methods = route->second;
        auto handler = methods.find(req.method);
        if (handler == methods.end() && head) handler = methods.find("GET");

        if (handler == methods.end()) {
            std::string allow;
            for (const auto& entry : methods) allow += (allow.empty() ? "" : ", ") + entry.first;
            Response r = make_error(405);
            r.headers.emplace_back("Allow", allow);
            return r;
        }
        Response r = handler->second.handler(req);
        r.omit_body = head;
        return r;
    }

    if (!static_root_.empty() && (req.method == "GET" || head)) {
        Response r = serve_file(path);
        r.omit_body = head;
        return r;
    }
    return make_error(404);
}
