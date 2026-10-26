#include "log.h"

#include <cstring>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace nfc {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
LogLevel g_level = LogLevel::Info;
bool g_debugger = true;

const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        default: return "OFF";
    }
}

void timestamp(char* out, std::size_t n) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(out, n, "%H:%M:%S", &tm);
}

}  // namespace

void logOpen(const std::string& path, LogLevel level, bool alsoDebugger) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_level = level;
    g_debugger = alsoDebugger;
    g_file = std::fopen(path.c_str(), "a");
    if (g_file) {
        char ts[16];
        timestamp(ts, sizeof(ts));
        std::fprintf(g_file, "\n==== %s nfsu2-coop %s ====\n", ts, versionString());
        std::fflush(g_file);
    }
}

void logClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void logSetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

LogLevel logLevel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

void logWrite(LogLevel level, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (static_cast<int>(level) < static_cast<int>(g_level)) return;

    char msg[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    char ts[16];
    timestamp(ts, sizeof(ts));

    if (g_file) {
        std::fprintf(g_file, "[%s][%-5s] %s\n", ts, levelName(level), msg);
        std::fflush(g_file);
    }
#ifdef _WIN32
    if (g_debugger) {
        char line[2200];
        std::snprintf(line, sizeof(line), "[nfsu2-coop][%s] %s\n", levelName(level), msg);
        OutputDebugStringA(line);
    }
#endif
}

}  // namespace nfc
