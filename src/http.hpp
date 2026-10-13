#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr size_t kMaxHeaderBytes = 8 * 1024;
constexpr size_t kMaxBodyBytes = 1024 * 1024;

struct Request {
    std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;  // names lowercased
    std::string body;
};

struct Response {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    bool omit_body = false;  // HEAD: send headers only
};

// Reads one full request off the socket.
// Returns 0 on success, -1 to drop silently, otherwise an HTTP error status.
int read_request(int fd, Request& req);

std::string serialize(const Response& res);
Response make_error(int status);
bool send_all(int fd, std::string_view data);
