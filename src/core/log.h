// Tiny leveled logger. Writes to a file and (optionally) to the debugger.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>
#include "common.h"

namespace nfc {

enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

void logOpen(const std::string& path, LogLevel level, bool alsoDebugger = true);
void logClose();
void logSetLevel(LogLevel level);
LogLevel logLevel();
void logWrite(LogLevel level, const char* fmt, ...);

#define NFC_TRACE(...) ::nfc::logWrite(::nfc::LogLevel::Trace, __VA_ARGS__)
#define NFC_DEBUG(...) ::nfc::logWrite(::nfc::LogLevel::Debug, __VA_ARGS__)
#define NFC_INFO(...) ::nfc::logWrite(::nfc::LogLevel::Info, __VA_ARGS__)
#define NFC_WARN(...) ::nfc::logWrite(::nfc::LogLevel::Warn, __VA_ARGS__)
#define NFC_ERROR(...) ::nfc::logWrite(::nfc::LogLevel::Error, __VA_ARGS__)

}  // namespace nfc
