#pragma once
// A deliberately tiny test framework: no dependencies, so the tests build with
// a plain g++ command as well as with CMake.
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

template <class A, class B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (a == b) return;
    std::ostringstream os;
    os << a << "  !=  " << b;
    std::printf("    FAILED %s:%d: %s == %s\n      got: %s\n", file, line, ea, eb, os.str().c_str());
    ++failures();
}

inline int run_all_tests() {
    int failed_tests = 0;
    for (const auto& t : registry()) {
        const int before = failures();
        t.fn();
        const bool ok = failures() == before;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", t.name);
        if (!ok) ++failed_tests;
    }
    std::printf("\n%zu tests, %d failed\n", registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}

#define TEST(name)                            \
    static void name();                       \
    static Registrar registrar_##name(#name, name); \
    static void name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures();                                                        \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b) check_eq((a), (b), #a, #b, __FILE__, __LINE__)
