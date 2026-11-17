#pragma once
#include <cstdarg>

namespace mge {
enum class LogLevel { Debug, Info, Warn, Error };
void logf(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
}

#define MGE_LOGD(...) ::mge::logf(::mge::LogLevel::Debug, __VA_ARGS__)
#define MGE_LOGI(...) ::mge::logf(::mge::LogLevel::Info, __VA_ARGS__)
#define MGE_LOGW(...) ::mge::logf(::mge::LogLevel::Warn, __VA_ARGS__)
#define MGE_LOGE(...) ::mge::logf(::mge::LogLevel::Error, __VA_ARGS__)
