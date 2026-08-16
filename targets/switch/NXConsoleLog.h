#pragma once

// Thread-safe logging for the libnx console.
//
// The libnx software console (printf -> framebuffer) is NOT thread-safe:
// concurrent access from a worker thread and the main thread's
// consoleUpdate() aborts the process inside framebufferBegin (confirmed by
// crash report, 2026-08-14). So: any thread may push formatted lines into
// the queue here; ONLY the main thread drains them to the real console.

#include <cstddef>
#include <string>

namespace nxlog {

// Safe from any thread
void push(const std::string& line);
void pushf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Main thread only: printf all queued lines. Returns true if any were printed
// (caller should consoleUpdate afterwards).
bool drain();

// Replace bell's default logger so all bell/cspot log output goes through
// the queue instead of printf'ing from worker threads.
void installBellLogger();

// Optional: also append every pushed line to a file (thread-safe, flushed
// per line). Used by the UI app, which has no console to drain to.
// maxBytes == 0 keeps the file unbounded for compatibility. Persistent
// services should provide a limit so diagnostics cannot fill the SD card.
void setLogFile(const char* path, size_t maxBytes = 0);

// Console apps drain the in-memory queue on their main thread. A headless
// sysmodule has no console consumer and should disable it after opening its
// file log.
void setQueueEnabled(bool enabled);

// Append fixed-heap allocator usage to the log without requiring a debugger.
void pushHeapUsage(const char* tag);

}  // namespace nxlog
