#include "test_main.h"
int main() {
    int failedTests = 0;
    for (auto& t : testRegistry()) {
        int before = testFailures();
        t.fn();
        bool ok = testFailures() == before;
        if (!ok) ++failedTests;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d failed\n", testRegistry().size(), failedTests);
    return failedTests ? 1 : 0;
}
