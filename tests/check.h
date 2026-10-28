#pragma once

// Minimal, dependency-free test harness: each test file defines TEST(name)
// blocks and the TEST_MAIN() at the bottom runs them all, printing one line
// per test and returning non-zero if any check failed. Kept deliberately tiny
// (no fixtures, no parametrization) -- the units under test here are pure
// DSP/containers, a handful of assertions is all they need.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char* name;
    std::function<void()> body;
};

inline std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back({ name, std::move(body) });
    }
};

inline int& failures() {
    static int count = 0;
    return count;
}

inline void fail(const char* file, int line, const std::string& what) {
    std::printf("    FAIL %s:%d: %s\n", file, line, what.c_str());
    ++failures();
}

} // namespace check

#define TEST(name) \
    static void test_##name(); \
    static check::Registrar registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) \
    do { if (!(cond)) check::fail(__FILE__, __LINE__, #cond); } while (0)

#define CHECK_EQ(a, b) \
    do { \
        auto va_ = (a); auto vb_ = (b); \
        if (!(va_ == vb_)) check::fail(__FILE__, __LINE__, \
            std::string(#a " == " #b " (") + std::to_string(va_) + " vs " + std::to_string(vb_) + ")"); \
    } while (0)

#define CHECK_NEAR(a, b, eps) \
    do { \
        double va_ = (a); double vb_ = (b); \
        if (std::fabs(va_ - vb_) > (eps)) check::fail(__FILE__, __LINE__, \
            std::string(#a " ~= " #b " (") + std::to_string(va_) + " vs " + std::to_string(vb_) + ")"); \
    } while (0)

#define TEST_MAIN() \
    int main() { \
        int failedTests = 0; \
        for (const auto& t : check::registry()) { \
            int before = check::failures(); \
            t.body(); \
            bool ok = check::failures() == before; \
            std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name); \
            if (!ok) ++failedTests; \
        } \
        std::printf("%zu tests, %d failed\n", check::registry().size(), failedTests); \
        return failedTests == 0 ? 0 : 1; \
    }
