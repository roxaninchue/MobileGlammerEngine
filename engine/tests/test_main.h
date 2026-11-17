#pragma once
// Minimal test harness (no third-party deps, works without exceptions).
#include <cstdio>
#include <vector>

struct TestCase { const char* name; void (*fn)(); };
inline std::vector<TestCase>& testRegistry() { static std::vector<TestCase> r; return r; }
inline int& testFailures() { static int f = 0; return f; }
struct TestRegistrar { TestRegistrar(const char* n, void (*f)()) { testRegistry().push_back({n, f}); } };

#define TEST(name) \
    static void test_##name(); \
    static TestRegistrar reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++testFailures(); } } while (0)
