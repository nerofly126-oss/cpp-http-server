#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr size_t kMaxHeaderBytes = 8 * 1024;
constexpr size_t kMaxBodyBytes = 1024 * 1024;
constexpr size_t kMaxHeaderCount = 100;

struct Request {
    std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;  // names lowercased
    std::string body;
    bool keep_alive = true;
};

struct Response {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    bool omit_body = false;  // HEAD: send headers only
};

enum class ParseStatus { Incomplete, Complete, Error };

struct ParseResult {
    ParseStatus status;
    int error;        // HTTP status to reply with when status == Error
    size_t consumed;  // bytes of the buffer used when status == Complete
};

// Tries to parse one request from the front of buf. Never blocks and never
// reads from a socket: the event loop feeds it whatever has arrived so far.
ParseResult parse_request(const std::string& buf, Request& req);

std::string serialize(const Response& res, bool keep_alive);
Response make_error(int status);
