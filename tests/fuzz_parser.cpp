// Fuzz target for the request parser, the router, and the response serializer.
//
// libFuzzer (needs clang), run from the project root so "static/" resolves:
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
//       tests/fuzz_parser.cpp src/http.cpp src/router.cpp -o fuzz_parser
//   ./fuzz_parser tests/corpus -max_total_time=60
//
// No clang? A plain g++ mutation loop works too:
//   g++ -std=c++17 -g -O1 -DFUZZ_STANDALONE -fsanitize=address,undefined \
//       tests/fuzz_parser.cpp src/http.cpp src/router.cpp -o fuzz_standalone
//   ./fuzz_standalone 2000000

#include <cstddef>
#include <cstdint>
#include <string>

#include "../src/http.hpp"
#include "../src/router.hpp"

static const Router& test_router() {
    static const Router router = [] {
        Router r;
        r.add("GET", "/health", [](const Request&) {
            Response x;
            x.body = "ok\n";
            return x;
        });
        r.add("POST", "/echo", [](const Request& q) {
            Response x;
            x.body = q.body;
            return x;
        });
        r.serve_static("static");
        return r;
    }();
    return router;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string buf(reinterpret_cast<const char*>(data), size);

    Request req;
    ParseResult pr = parse_request(buf, req);

    if (pr.status == ParseStatus::Complete) {
        if (pr.consumed == 0 || pr.consumed > buf.size()) __builtin_trap();
        if (req.method.empty() || req.target.empty() || req.target[0] != '/') __builtin_trap();
        Response res = test_router().dispatch(req);
        std::string wire = serialize(res, req.keep_alive);
        if (wire.compare(0, 9, "HTTP/1.1 ") != 0) __builtin_trap();
    } else if (pr.status == ParseStatus::Error) {
        if (pr.error < 400 || pr.error > 599) __builtin_trap();
    }
    return 0;
}

#ifdef FUZZ_STANDALONE
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::atol(argv[1]) : 1000000;
    std::vector<std::string> seeds = {
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
        "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",
        "GET /../src/main.cpp HTTP/1.1\r\nConnection: close\r\n\r\n",
        "GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.0\r\n\r\n",
        "HEAD /index.html?x=1 HTTP/1.1\r\nA: b\r\nC: d\r\n\r\n",
        "GET /health HTTP/1.1\r\n\r\nGET /health HTTP/1.1\r\n\r\n",
    };
    const std::string alphabet = "\r\n :/%.-0123456789aAzZ\t\x01\x7f\xff";
    std::mt19937_64 rng(12345);
    auto pick = [&](size_t n) { return static_cast<size_t>(rng() % n); };

    for (long i = 0; i < iterations; ++i) {
        std::string s = seeds[pick(seeds.size())];
        const int mutations = 1 + static_cast<int>(pick(6));
        for (int m = 0; m < mutations; ++m) {
            switch (pick(5)) {
                case 0:  // overwrite a byte
                    if (!s.empty()) s[pick(s.size())] = static_cast<char>(rng());
                    break;
                case 1:  // insert an interesting byte
                    s.insert(pick(s.size() + 1), 1, alphabet[pick(alphabet.size())]);
                    break;
                case 2:  // delete a span
                    if (!s.empty()) s.erase(pick(s.size()), 1 + pick(8));
                    break;
                case 3:  // duplicate a span
                    if (!s.empty()) {
                        size_t at = pick(s.size());
                        s.insert(pick(s.size() + 1), s.substr(at, 1 + pick(32)));
                    }
                    break;
                case 4:  // splice in a snippet from another seed
                    s.insert(pick(s.size() + 1), seeds[pick(seeds.size())].substr(0, pick(40)));
                    break;
            }
        }
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    std::printf("ran %ld inputs, no crashes\n", iterations);
    return 0;
}
#endif
