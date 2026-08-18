#include "background_backend.hpp"
#include "ipc_self_test.hpp"
#include "switify_service.hpp"

#include "NXConsoleLog.h"

#include <curl/curl.h>
#include <switch.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <new>

namespace {

constexpr SocketInitConfig SocketConfig = {
    .tcp_tx_buf_size = 0x800,
    .tcp_rx_buf_size = 0x1000,
    .tcp_tx_buf_max_size = 0x2EE0,
    .tcp_rx_buf_max_size = 0x4000,
    // Spotify AP, Web API, and CDN traffic are TCP-only. Reserving UDP
    // buffers and six bsd:u sessions bought us nothing, but the sessions stay
    // claimed for the entire sysmodule lifetime. Nintendo's online emulators
    // need the same service while launching. Match Streamfin's proven
    // background profile: two BSD sessions and no UDP allocation.
    .udp_tx_buf_size = 0,
    .udp_rx_buf_size = 0,
    .sb_efficiency = 2,
    .num_bsd_sessions = 2,
    .bsd_service_type = BsdServiceType_User,
};

Result socketInitResult = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

void abortUnless(Result result) {
  if (R_FAILED(result)) diagAbortWithResult(result);
}

}  // namespace

extern "C" {

uint32_t __nx_applet_type = AppletType_None;
uint32_t __nx_fs_num_sessions = 2;

void __libnx_initheap(void) {
  // Recovery profile: retain the last hardware-proven ceiling. Compatibility
  // reductions must be reintroduced one at a time after runtime measurement.
#ifdef SWITIFY_LIGHTWEIGHT_OVERLAY
  // Hardware logs peak around a 2.2 MiB arena.  Three MiB retains meaningful
  // headroom while reducing the process image reservation by one MiB.
  static char heap[3 * 1024 * 1024];
#else
  static char heap[4 * 1024 * 1024];
#endif
  extern char* fake_heap_start;
  extern char* fake_heap_end;
  fake_heap_start = heap;
  fake_heap_end = heap + sizeof(heap);
}

void __appInit(void) {
  abortUnless(smInitialize());
  abortUnless(fsInitialize());
  abortUnless(fsdevMountSdmc());
  socketInitResult = socketInitialize(&SocketConfig);
}

void __appExit(void) {
  curl_global_cleanup();
  if (R_SUCCEEDED(socketInitResult)) socketExit();
  fsdevUnmountDevice("sdmc");
  fsExit();
  smExit();
}

}  // extern "C"

int main(int, char**) {
  mkdir("/config", 0755);
  mkdir("/config/Switify", 0755);
  nxlog::installBellLogger();
  nxlog::setLogFile("/config/Switify/background.log", 512 * 1024);
  nxlog::setQueueEnabled(false);
#ifdef SWITIFY_LIGHTWEIGHT_OVERLAY
  nxlog::push(
      "Switify Phase 2 background engine 0.4.3-lite1 starting");
  nxlog::push("Lightweight profile: 3072 KiB heap, Vorbis-only decoder");
#else
  nxlog::push("Switify Phase 2 background engine 0.4.3 starting");
#endif
  nxlog::push(
      "Compatibility profile: Applet pool, lazy audout, 2 BSD sessions");
  nxlog::push("Network profile: TCP-only; UDP disabled");
  nxlog::pushHeapUsage("startup");

  // ServiceCore owns the fixed-capacity browsing snapshot and is far larger
  // than the 16 KiB main-thread stack. Keep all long-lived engine state on the
  // process heap so adding browse fields cannot silently overflow that stack.
  auto core = std::unique_ptr<switify::phase2::ServiceCore>(
      new (std::nothrow) switify::phase2::ServiceCore());
  if (!core)
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
  nxlog::pushHeapUsage("service core ready");
  abortUnless(switify::phase2::switch_service::initialize(*core));

  auto backend = std::unique_ptr<switify::phase2::BackgroundBackend>(
      new (std::nothrow) switify::phase2::BackgroundBackend(*core));
  if (!backend)
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
  bool backendStarted = false;
  if (R_SUCCEEDED(socketInitResult)) {
    backendStarted = backend->start();
    if (!backendStarted) diagAbortWithResult(MAKERESULT(421, 10));
    nxlog::pushHeapUsage("Spotify backend started");
  } else {
    char detail[40];
    std::snprintf(detail, sizeof(detail), "Network unavailable: 0x%08X",
                  socketInitResult);
    core->configureCommands(false, SwitifyIpcCapability_Status, 0);
    core->publishServiceState(SwitifyServiceState_Error, 0);
    core->publishTrack({}, {}, detail, 0);
    nxlog::pushf("Socket initialization failed: 0x%08X", socketInitResult);
  }
  abortUnless(switify::phase2::ipc_self_test::start());

  switify::phase2::switch_service::loop();

  if (backendStarted) backend->stop();
  abortUnless(switify::phase2::switch_service::exit());
  return 0;
}
