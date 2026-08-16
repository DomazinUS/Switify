#include "switify_service.hpp"
#include "ipc_self_test.hpp"

#include "switify/service_core.hpp"

#include <switch.h>

namespace {

bool fsMounted = false;

void abortUnless(Result result) {
  if (R_FAILED(result)) diagAbortWithResult(result);
}

}  // namespace

extern "C" {

uint32_t __nx_applet_type = AppletType_None;
uint32_t __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
  static char heap[256 * 1024];
  extern char* fake_heap_start;
  extern char* fake_heap_end;
  fake_heap_start = heap;
  fake_heap_end = heap + sizeof(heap);
}

void __appInit(void) {
  abortUnless(smInitialize());
  if (R_SUCCEEDED(fsInitialize())) {
    if (R_SUCCEEDED(fsdevMountSdmc()))
      fsMounted = true;
    else
      fsExit();
  }
}

void __appExit(void) {
  if (fsMounted) {
    fsdevUnmountDevice("sdmc");
    fsExit();
    fsMounted = false;
  }
  smExit();
}

}  // extern "C"

int main(int, char**) {
  switify::phase2::ServiceCore core;
  core.publishServiceState(SwitifyServiceState_Booting, 0);
  core.configureCommands(false, SwitifyIpcCapability_Status, 0);

  abortUnless(switify::phase2::switch_service::initialize(core));
  abortUnless(switify::phase2::ipc_self_test::start());
  switify::phase2::switch_service::loop();
  core.publishServiceState(SwitifyServiceState_Stopping, 0);
  abortUnless(switify::phase2::switch_service::exit());
  return 0;
}
