#include "NXConsoleLog.h"

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <malloc.h>
#include <mutex>
#include <thread>

#include "BellLogger.h"

namespace {

std::mutex logMutex;
std::deque<std::string> logLines;
FILE* logFile = nullptr;
std::string logPath;
size_t logFileBytes = 0;
size_t logFileLimit = 0;
bool queueEnabled = true;
constexpr size_t MAX_QUEUED = 2048;  // keep useful pre-crash context
const auto logEpoch = std::chrono::steady_clock::now();

std::string basename(const std::string& filename) {
  return filename.substr(filename.rfind('/') + 1);
}

// bell/cspot logger that formats into the queue instead of printf'ing
class NXQueueLogger : public bell::AbstractLogger {
 public:
  void debug(std::string filename, int line, std::string submodule,
             const char* format, ...) override {
    va_list args;
    va_start(args, format);
    log('D', filename, line, format, args);
    va_end(args);
  }
  void error(std::string filename, int line, std::string submodule,
             const char* format, ...) override {
    va_list args;
    va_start(args, format);
    log('E', filename, line, format, args);
    va_end(args);
  }
  void info(std::string filename, int line, std::string submodule,
            const char* format, ...) override {
    va_list args;
    va_start(args, format);
    log('I', filename, line, format, args);
    va_end(args);
  }

 private:
  void log(char level, const std::string& filename, int line,
           const char* format, va_list args) {
    char msg[512];
    vsnprintf(msg, sizeof(msg), format, args);
    char full[640];
    snprintf(full, sizeof(full), "%c %s:%d: %s", level,
             basename(filename).c_str(), line, msg);
    nxlog::push(full);
  }
};

}  // namespace

namespace nxlog {

void push(const std::string& line) {
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - logEpoch)
                             .count();
  const size_t threadTag =
      std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff;
  char prefix[48];
  snprintf(prefix, sizeof(prefix), "[%08lld ms][t%04zx] ",
           static_cast<long long>(elapsedMs), threadTag);
  const std::string decorated = std::string(prefix) + line;

  std::lock_guard<std::mutex> lock(logMutex);
  if (queueEnabled) {
    if (logLines.size() >= MAX_QUEUED)
      logLines.pop_front();
    logLines.push_back(decorated);
  }
  if (logFile) {
    const size_t bytes = decorated.size() + 1;
    if (logFileLimit != 0 && logFileBytes + bytes > logFileLimit &&
        !logPath.empty()) {
      fclose(logFile);
      logFile = nullptr;
      const std::string previous = logPath + ".previous";
      std::remove(previous.c_str());
      std::rename(logPath.c_str(), previous.c_str());
      logFile = fopen(logPath.c_str(), "w");
      logFileBytes = 0;
    }
    if (!logFile) return;
    fputs(decorated.c_str(), logFile);
    fputc('\n', logFile);
    fflush(logFile);
    logFileBytes += bytes;
  }
}

void pushf(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  push(buf);
}

bool drain() {
  std::deque<std::string> pending;
  {
    std::lock_guard<std::mutex> lock(logMutex);
    pending.swap(logLines);
  }
  for (auto& line : pending) {
    printf("%s\n", line.c_str());
  }
  return !pending.empty();
}

void installBellLogger() {
  bell::bellGlobalLogger = new NXQueueLogger();
}

void setLogFile(const char* path, size_t maxBytes) {
  std::lock_guard<std::mutex> lock(logMutex);
  if (logFile)
    fclose(logFile);
  // Preserve the previous launch automatically. This matters because
  // Atmosphère returns to hbmenu after a crash and the next launch would
  // otherwise truncate the only useful runtime context.
  const std::string previous = std::string(path) + ".previous";
  std::remove(previous.c_str());
  std::rename(path, previous.c_str());
  logFile = fopen(path, "w");
  logPath = path;
  logFileBytes = 0;
  logFileLimit = maxBytes;
}

void setQueueEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(logMutex);
  queueEnabled = enabled;
  if (!enabled) logLines.clear();
}

void pushHeapUsage(const char* tag) {
  const struct mallinfo info = mallinfo();
  pushf("Heap [%s]: used=%zu KiB free=%zu KiB arena=%zu KiB blocks=%zu",
        tag ? tag : "unknown", info.uordblks / 1024, info.fordblks / 1024,
        info.arena / 1024, info.ordblks);
}

}  // namespace nxlog
