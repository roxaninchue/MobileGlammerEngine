#pragma once
#include "mge/log.h"

namespace mge {
[[noreturn]] void assertFailed(const char* expr, const char* file, int line, const char* msg);
}

#if defined(NDEBUG) && !defined(MGE_FORCE_ASSERTS)
#define MGE_ASSERT(expr, ...) ((void)0)
#else
#define MGE_ASSERT(expr, ...) \
    ((expr) ? (void)0 : ::mge::assertFailed(#expr, __FILE__, __LINE__, "" __VA_ARGS__))
#endif
// Always-on check for conditions that must hold even in release.
#define MGE_VERIFY(expr, ...) \
    ((expr) ? (void)0 : ::mge::assertFailed(#expr, __FILE__, __LINE__, "" __VA_ARGS__))
