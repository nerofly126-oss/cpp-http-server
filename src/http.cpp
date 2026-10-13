#include "http.hpp"

#include <sys/socket.h>

#include <cctype>
#include <cerrno>

// ------------------------------------------------------------------ helpers

static std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

// ------------------------------------------------------------------ parsing

// Parses the request line and headers. Returns 0, or the status to reply with.
static int parse_head(std::string_view head, Request& req) {
    const size_t npos = std::string_view::npos;

    size_t eol = head.find("\r\n");
    std::string_view line = head.substr(0, eol);

    size_t sp1 = line.find(' ');
    size_t sp2 = (sp1 == npos) ? npos : line.find(' ', sp1 + 1);
    if (sp1 == npos || sp2 == npos) return 400;

    req.method = std::string(line.substr(0, sp1));
    req.target = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
    req.version = std::string(line.substr(sp2 + 1));

    if (req.method.empty() || req.target.empty() || req.target[0] != '/') return 400;
    if (req.version.compare(0, 5, "HTTP/") != 0) return 400;
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") return 505;

    std::string_view rest = (eol == npos) ? std::string_view{} : head.substr(eol + 2);
    while (!rest.empty()) {
        size_t e = rest.find("\r\n");
        std::string_view h = rest.substr(0, e);
        rest = (e == npos) ? std::string_view{} : rest.substr(e + 2);

        size_t colon = h.find(':');
        if (colon == npos || colon == 0) return 400;
        std::string_view name = h.substr(0, colon);
        if (name.find_first_of(" \t") != npos) return 400;

        req.headers[to_lower(std::string(name))] = std::string(trim(h.substr(colon + 1)));
    }
    return 0;
}

// Reads at least one more byte. 0 = ok, 408 = timeout, -1 = peer closed/errored.
static int recv_more(int fd, std::string& buf) {
    char chunk[4096];
    for (;;) {
        ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n > 0) {
            buf.append(chunk, static_cast<size_t>(n));
            return 0;
        }
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 408;
        return -1;
    }
}

int read_request(int fd, Request& req) {
    const size_t npos = std::string::npos;
    std::string buf;
    size_t head_end;

    while ((head_end = buf.find("\r\n\r\n")) == npos) {
        if (buf.size() > kMaxHeaderBytes) return 431;
        if (int rc = recv_more(fd, buf)) return rc;
    }
    if (head_end > kMaxHeaderBytes) return 431;

    if (int rc = parse_head(std::string_view(buf).substr(0, head_end), req)) return rc;
    if (req.headers.count("transfer-encoding")) return 501;

    size_t content_length = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        const std::string& v = it->second;
        if (v.empty() || v.size() > 10 || v.find_first_not_of("0123456789") != npos) return 400;
        content_length = std::stoul(v);
        if (content_length > kMaxBodyBytes) return 413;
    }

    req.body = buf.substr(head_end + 4);
    while (req.body.size() < content_length) {
        if (int rc = recv_more(fd, req.body)) return rc;
    }
    req.body.resize(content_length);
    return 0;
}

// ---------------------------------------------------------------- responses

static const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

std::string serialize(const Response& res) {
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) + "\r\n";
    out += "Content-Type: " + res.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    for (const auto& [name, value] : res.headers) out += name + ": " + value + "\r\n";
    out += "\r\n";
    if (!res.omit_body) out += res.body;
    return out;
}

Response make_error(int status) {
    Response r;
    r.status = status;
    r.body = std::to_string(status) + " " + reason(status) + "\n";
    return r;
}
