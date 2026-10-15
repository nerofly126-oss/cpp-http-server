#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "conn_limiter.hpp"
#include "http.hpp"
#include "router.hpp"
#include "test.hpp"

namespace fs = std::filesystem;

// ------------------------------------------------------------------ helpers

static Request parse_ok(const std::string& raw, size_t* consumed = nullptr) {
    Request req;
    ParseResult pr = parse_request(raw, req);
    CHECK(pr.status == ParseStatus::Complete);
    if (consumed) *consumed = pr.consumed;
    return req;
}

// Returns the HTTP error status, or 0 if the input did not produce an error.
static int parse_error(const std::string& raw) {
    Request req;
    ParseResult pr = parse_request(raw, req);
    return pr.status == ParseStatus::Error ? pr.error : 0;
}

static Request make_request(const std::string& method, const std::string& target) {
    Request req;
    req.method = method;
    req.target = target;
    req.version = "HTTP/1.1";
    return req;
}

static std::string header_value(const Response& res, const std::string& name) {
    for (const auto& [k, v] : res.headers)
        if (k == name) return v;
    return "";
}

static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static Response text_response(const std::string& body) {
    Response r;
    r.body = body;
    return r;
}

// ------------------------------------------------------------------- parser

TEST(parser_waits_for_complete_headers) {
    Request req;
    CHECK(parse_request("", req).status == ParseStatus::Incomplete);
    CHECK(parse_request("GET / HTTP/1.1\r\nHost: x\r\n", req).status == ParseStatus::Incomplete);
}

TEST(parser_reads_a_simple_get) {
    const std::string raw =
        "GET /index.html?x=1 HTTP/1.1\r\nHost: Example.com\r\nX-Thing:   padded value  \r\n\r\n";
    size_t consumed = 0;
    Request r = parse_ok(raw, &consumed);
    CHECK_EQ(r.method, "GET");
    CHECK_EQ(r.target, "/index.html?x=1");
    CHECK_EQ(r.version, "HTTP/1.1");
    CHECK_EQ(r.headers.at("host"), "Example.com");      // names lowercased
    CHECK_EQ(r.headers.at("x-thing"), "padded value");  // values trimmed
    CHECK_EQ(consumed, raw.size());
    CHECK(r.keep_alive);
}

TEST(parser_is_incomplete_at_every_prefix_of_a_body) {
    const std::string full = "POST /echo HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world";
    for (size_t cut = 0; cut < full.size(); ++cut) {
        Request req;
        if (parse_request(full.substr(0, cut), req).status != ParseStatus::Incomplete) {
            std::printf("    prefix of %zu bytes was not Incomplete\n", cut);
            ++failures();
        }
    }
    Request r = parse_ok(full);
    CHECK_EQ(r.body, "hello world");
}

TEST(parser_splits_pipelined_requests) {
    const std::string a = "GET /a HTTP/1.1\r\n\r\n";
    const std::string b = "POST /b HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi";
    const std::string buf = a + b;

    size_t used = 0;
    Request first = parse_ok(buf, &used);
    CHECK_EQ(first.target, "/a");
    CHECK_EQ(used, a.size());

    size_t used2 = 0;
    Request second = parse_ok(buf.substr(used), &used2);
    CHECK_EQ(second.target, "/b");
    CHECK_EQ(second.body, "hi");
    CHECK_EQ(used2, b.size());
}

TEST(parser_keep_alive_rules) {
    struct Case {
        const char* raw;
        bool keep_alive;
    };
    const Case cases[] = {
        {"GET / HTTP/1.1\r\n\r\n", true},
        {"GET / HTTP/1.1\r\nConnection: close\r\n\r\n", false},
        {"GET / HTTP/1.1\r\nConnection: Close\r\n\r\n", false},
        {"GET / HTTP/1.0\r\n\r\n", false},
        {"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", true},
    };
    for (const auto& c : cases) {
        Request r = parse_ok(c.raw);
        if (r.keep_alive != c.keep_alive) {
            std::printf("    wrong keep_alive for: %s\n", c.raw);
            ++failures();
        }
    }
}

TEST(parser_rejects_bad_requests) {
    struct Case {
        const char* name;
        std::string raw;
        int status;
    };
    const std::string nul_header = std::string("GET / HTTP/1.1\r\nX: a") + '\0' + "b\r\n\r\n";
    const Case cases[] = {
        {"garbage", "garbage\r\n\r\n", 400},
        {"missing leading slash", "GET index HTTP/1.1\r\n\r\n", 400},
        {"not an HTTP version", "GET / HTP/1.1\r\n\r\n", 400},
        {"unsupported version", "GET / HTTP/2.0\r\n\r\n", 505},
        {"space in version", "GET / HTTP/1.1 junk\r\n\r\n", 400},
        {"space before colon", "GET / HTTP/1.1\r\nBad Header: x\r\n\r\n", 400},
        {"nameless header", "GET / HTTP/1.1\r\n: nameless\r\n\r\n", 400},
        {"NUL in header", nul_header, 400},
        {"bare LF in header", "GET / HTTP/1.1\r\nX: a\nInjected: b\r\n\r\n", 400},
        {"non-numeric length", "POST /e HTTP/1.1\r\nContent-Length: abc\r\n\r\n", 400},
        {"negative length", "POST /e HTTP/1.1\r\nContent-Length: -5\r\n\r\n", 400},
        {"body too large", "POST /e HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n", 413},
        {"chunked not supported", "POST /e HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        {"conflicting lengths",
         "POST /e HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd", 400},
    };
    for (const auto& c : cases) {
        const int got = parse_error(c.raw);
        if (got != c.status) {
            std::printf("    case '%s': expected %d, got %d\n", c.name, c.status, got);
            ++failures();
        }
    }
}

TEST(parser_accepts_identical_duplicate_content_length) {
    Request r = parse_ok("POST /e HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc");
    CHECK_EQ(r.body, "abc");
}

TEST(parser_enforces_size_limits) {
    // Header block that never ends.
    CHECK_EQ(parse_error("GET / HTTP/1.1\r\nX: " + std::string(kMaxHeaderBytes + 10, 'a')), 431);
    // Header block that ends, but is too big.
    CHECK_EQ(parse_error("GET / HTTP/1.1\r\nX: " + std::string(kMaxHeaderBytes + 10, 'a') + "\r\n\r\n"),
             431);

    auto with_headers = [](size_t n) {
        std::string raw = "GET / HTTP/1.1\r\n";
        for (size_t i = 0; i < n; ++i) raw += "H" + std::to_string(i) + ": v\r\n";
        return raw + "\r\n";
    };
    CHECK_EQ(parse_error(with_headers(kMaxHeaderCount)), 0);
    CHECK_EQ(parse_error(with_headers(kMaxHeaderCount + 1)), 431);

    Request req;
    const std::string at_limit =
        "POST /e HTTP/1.1\r\nContent-Length: " + std::to_string(kMaxBodyBytes) + "\r\n\r\n";
    CHECK(parse_request(at_limit, req).status == ParseStatus::Incomplete);
    const std::string over_limit =
        "POST /e HTTP/1.1\r\nContent-Length: " + std::to_string(kMaxBodyBytes + 1) + "\r\n\r\n";
    CHECK_EQ(parse_error(over_limit), 413);
}

// --------------------------------------------------------------- serializer

TEST(serializer_writes_length_and_connection_headers) {
    Response r = text_response("hello");
    const std::string keep = serialize(r, true);
    CHECK(keep.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(keep.find("Content-Length: 5\r\n") != std::string::npos);
    CHECK(keep.find("Connection: keep-alive\r\n") != std::string::npos);
    CHECK(ends_with(keep, "\r\n\r\nhello"));

    const std::string close = serialize(r, false);
    CHECK(close.find("Connection: close\r\n") != std::string::npos);
}

TEST(serializer_omits_body_for_head_but_keeps_length) {
    Response r = text_response("hello");
    r.omit_body = true;
    const std::string wire = serialize(r, true);
    CHECK(wire.find("Content-Length: 5\r\n") != std::string::npos);
    CHECK(ends_with(wire, "\r\n\r\n"));
}

TEST(make_error_has_matching_body) {
    Response r = make_error(404);
    CHECK_EQ(r.status, 404);
    CHECK_EQ(r.body, "404 Not Found\n");
}

// ------------------------------------------------------------------- router

TEST(router_dispatches_by_method_and_path) {
    Router router;
    router.add("GET", "/a", [](const Request&) { return text_response("get-a"); });
    router.add("POST", "/a", [](const Request&) { return text_response("post-a"); });

    CHECK_EQ(router.dispatch(make_request("GET", "/a")).body, "get-a");
    CHECK_EQ(router.dispatch(make_request("POST", "/a")).body, "post-a");
    CHECK_EQ(router.dispatch(make_request("GET", "/missing")).status, 404);
}

TEST(router_answers_405_with_allow_header) {
    Router router;
    router.add("GET", "/a", [](const Request&) { return text_response("x"); });
    router.add("POST", "/a", [](const Request&) { return text_response("x"); });

    Response r = router.dispatch(make_request("DELETE", "/a"));
    CHECK_EQ(r.status, 405);
    CHECK_EQ(header_value(r, "Allow"), "GET, POST");
}

TEST(router_serves_head_from_the_get_handler) {
    Router router;
    router.add("GET", "/a", [](const Request&) { return text_response("body"); });

    Response r = router.dispatch(make_request("HEAD", "/a"));
    CHECK_EQ(r.status, 200);
    CHECK(r.omit_body);
}

TEST(router_ignores_the_query_string) {
    Router router;
    router.add("GET", "/a", [](const Request&) { return text_response("ok"); });
    CHECK_EQ(router.dispatch(make_request("GET", "/a?x=1&y=2")).status, 200);
}

TEST(router_decodes_percent_escapes) {
    Router router;
    router.add("GET", "/a b", [](const Request&) { return text_response("ok"); });
    CHECK_EQ(router.dispatch(make_request("GET", "/a%20b")).status, 200);
    CHECK_EQ(router.dispatch(make_request("GET", "/a%zz")).status, 400);
    CHECK_EQ(router.dispatch(make_request("GET", "/a%4")).status, 400);
}

TEST(router_runs_cheap_handlers_on_the_loop_and_the_rest_on_workers) {
    Router router;
    router.add("GET", "/fast", [](const Request&) { return text_response("x"); }, RunOn::Loop);
    router.add("GET", "/slow", [](const Request&) { return text_response("x"); });  // default: Worker
    router.serve_static("static");

    CHECK(router.runs_on_loop(make_request("GET", "/fast")));
    CHECK(router.runs_on_loop(make_request("HEAD", "/fast")));
    CHECK(router.runs_on_loop(make_request("GET", "/fast?x=1")));
    CHECK(!router.runs_on_loop(make_request("GET", "/slow")));
    CHECK(!router.runs_on_loop(make_request("GET", "/index.html")));  // static file: may touch disk
    CHECK(!router.runs_on_loop(make_request("GET", "/missing")));
    CHECK(router.runs_on_loop(make_request("DELETE", "/fast")));      // 405 is cheap
    CHECK(router.runs_on_loop(make_request("GET", "/fast%zz")));      // 400 is cheap
}

// ---------------------------------------------------------- connection limits

TEST(limiter_enforces_per_ip_and_total_caps) {
    ConnLimiter limiter(5, 2);
    CHECK(limiter.try_acquire(1));
    CHECK(limiter.try_acquire(1));
    CHECK(!limiter.try_acquire(1));  // third from the same address
    CHECK(limiter.try_acquire(2));   // a different address is unaffected
    CHECK(limiter.try_acquire(2));
    CHECK(limiter.try_acquire(3));
    CHECK_EQ(limiter.total(), static_cast<size_t>(5));
    CHECK(!limiter.try_acquire(4));  // total cap reached

    limiter.release(1);
    CHECK(limiter.try_acquire(4));   // a slot opened up
    CHECK(limiter.try_acquire(1) == false);  // but the total is full again
    CHECK_EQ(limiter.total(), static_cast<size_t>(5));
}

TEST(limiter_stays_consistent_under_concurrent_use) {
    ConnLimiter limiter(1000, 50);
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < 8; ++t) {
        threads.emplace_back([&limiter, t] {
            for (int i = 0; i < 20000; ++i) {
                const uint32_t ip = (t + static_cast<uint32_t>(i)) % 4;
                if (limiter.try_acquire(ip)) limiter.release(ip);
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(limiter.total(), static_cast<size_t>(0));
}

// ------------------------------------------------------------ static files

// A throwaway web root with files inside it, plus a secret and two symlinks
// that try to escape from it.
struct StaticFixture {
    fs::path base;
    Router router;

    StaticFixture() {
        base = fs::temp_directory_path() / ("httpserver-unit-" + std::to_string(getpid()));
        fs::remove_all(base);
        const fs::path root = base / "root";
        fs::create_directories(root / "sub");
        fs::create_directories(root / "emptydir");
        write(root / "index.html", "<h1>hi</h1>");
        write(root / "sub" / "page.css", "body{}");
        write(root / "data.bin", "\x01\x02");
        write(base / "secret.txt", "top secret");
        fs::create_symlink("../secret.txt", root / "leak.txt");
        fs::create_directory_symlink(base, root / "dirlink");
        router.serve_static(root.string());
    }
    ~StaticFixture() { fs::remove_all(base); }

    static void write(const fs::path& p, const std::string& content) {
        std::ofstream(p, std::ios::binary) << content;
    }
};

TEST(static_serves_files_with_mime_types) {
    StaticFixture fx;

    Response index = fx.router.dispatch(make_request("GET", "/"));
    CHECK_EQ(index.status, 200);
    CHECK_EQ(index.body, "<h1>hi</h1>");
    CHECK(index.content_type.rfind("text/html", 0) == 0);

    Response css = fx.router.dispatch(make_request("GET", "/sub/page.css"));
    CHECK_EQ(css.status, 200);
    CHECK(css.content_type.rfind("text/css", 0) == 0);

    Response bin = fx.router.dispatch(make_request("GET", "/data.bin"));
    CHECK_EQ(bin.content_type, "application/octet-stream");

    CHECK_EQ(fx.router.dispatch(make_request("GET", "/index.html?cache=bust")).status, 200);
}

TEST(static_returns_404_for_missing_files_and_bare_directories) {
    StaticFixture fx;
    CHECK_EQ(fx.router.dispatch(make_request("GET", "/nope.txt")).status, 404);
    CHECK_EQ(fx.router.dispatch(make_request("GET", "/emptydir")).status, 404);
}

TEST(static_blocks_every_way_out_of_the_web_root) {
    StaticFixture fx;
    const char* attempts[] = {
        "/../secret.txt",
        "/%2e%2e/secret.txt",
        "/sub/../../secret.txt",
        "/%2e%2e/%2e%2e/etc/passwd",
        "//etc/passwd",
        "/leak.txt",              // symlink to a file outside the root
        "/dirlink/secret.txt",    // symlink to a directory outside the root
    };
    for (const char* target : attempts) {
        const int status = fx.router.dispatch(make_request("GET", target)).status;
        if (status != 403) {
            std::printf("    %s returned %d instead of 403\n", target, status);
            ++failures();
        }
    }
}

TEST(static_rejects_embedded_nul_bytes) {
    StaticFixture fx;
    CHECK_EQ(fx.router.dispatch(make_request("GET", "/a%00b")).status, 400);
}

TEST(static_head_sets_omit_body) {
    StaticFixture fx;
    Response r = fx.router.dispatch(make_request("HEAD", "/index.html"));
    CHECK_EQ(r.status, 200);
    CHECK(r.omit_body);
}

int main() { return run_all_tests(); }
