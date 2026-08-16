#include "switify/ipc_client.h"

#include <stdio.h>
#include <string.h>

#define SWITIFY_PROGRAM_ID UINT64_C(0x4200000000535754)

static bool service_missing = false;

static void printResult(const char* operation, Result result) {
  printf("%s: 0x%08X (module %u, description %u)\n", operation, result,
         R_MODULE(result), R_DESCRIPTION(result));
}

static bool connectToService(void) {
  if (switifyIpcIsActive()) return true;
  Result result = switifyIpcInitialize();
  if (R_SUCCEEDED(result)) return true;
  service_missing = true;
  printResult("Service connection unavailable", result);
  if (result == KERNELRESULT(OutOfResource))
    printf("The IPC port is busy or left in a stale state.\n");
  printf("\nThe installed service is dormant.\n");
  printf("Press A to launch it for this session only.\n");
  return false;
}

static bool launchService(void) {
  printf("\n--- On-demand launch attempt ---\n");
  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    printResult("Open pm:shell failed", result);
    return false;
  }

  uint64_t existing_pid = 0;
  const Result lookup_result =
      pmshellGetProcessId(&existing_pid, SWITIFY_PROGRAM_ID);
  if (R_SUCCEEDED(lookup_result) && existing_pid != 0) {
    printf("Program is already running as PID %lu, but IPC is unavailable.\n",
           existing_pid);
    printf("Press X to terminate it, then press A to retry.\n");
    pmshellExit();
    return false;
  }

  const NcmProgramLocation location = {
      .program_id = SWITIFY_PROGRAM_ID,
      .storageID = NcmStorageId_None,
  };
  uint64_t pid = 0;
  result = pmshellLaunchProgram(0, &location, &pid);
  pmshellExit();
  if (R_FAILED(result)) {
    printResult("Launch request failed", result);
    printf("Atmosphere may need one reboot to discover the new content.\n");
    return false;
  }
  printf("Launch request succeeded. PID: %lu\n", pid);

  for (unsigned attempt = 0; attempt < 50; ++attempt) {
    svcSleepThread(UINT64_C(10000000));
    result = switifyIpcInitialize();
    if (R_SUCCEEDED(result)) {
      service_missing = false;
      printf("IPC became available after %u ms.\n", (attempt + 1) * 10);
      return true;
    }
  }
  printResult("IPC after 500 ms failed", result);
  printf("The process either exited or did not publish its service.\n");
  return false;
}

static void stopService(void) {
  switifyIpcExit();
  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    printResult("pmshellInitialize", result);
    return;
  }
  result = pmshellTerminateProgram(SWITIFY_PROGRAM_ID);
  pmshellExit();
  if (R_FAILED(result))
    printResult("Stop request failed", result);
  else
    printf("Switify service stopped. It will remain off after reboot.\n");
  service_missing = true;
}

static void refresh(void) {
  service_missing = false;
  printf("\x1b[2J\x1b[H");
  printf("Switify Phase 2 service probe\n");
  printf("================================\n\n");

  if (!connectToService()) return;

  uint32_t version = 0;
  Result result = switifyIpcGetApiVersion(&version);
  if (R_FAILED(result)) {
    printResult("GetApiVersion", result);
    return;
  }

  uint64_t capabilities = 0;
  result = switifyIpcGetCapabilities(&capabilities);
  if (R_FAILED(result)) {
    printResult("GetCapabilities", result);
    return;
  }

  SwitifyIpcStatus status;
  memset(&status, 0, sizeof(status));
  result = switifyIpcGetStatus(&status);
  if (R_FAILED(result)) {
    printResult("GetStatus", result);
    return;
  }

  printf("Connected to port: %s\n", SWITIFY_IPC_SERVICE_NAME);
  printf("API: %u.%u (0x%08X)\n", version >> 16u, version & 0xFFFFu,
         version);
  printf("Capabilities: 0x%016lX\n", capabilities);
  printf("Status size / ABI: %u / %u\n", status.struct_size,
         status.abi_version);
  printf("Sequence: %lu\n", status.sequence);
  printf("Service / playback: %u / %u\n", status.service_state,
         status.playback_state);
  printf("Last control result: %u\n", status.last_result);
  printf("\nExpected health-build values:\n");
  printf("  API 1.0, capabilities 0x1, status 216 / 1\n");
  printf("  service 0, playback 0, last result 3\n");

  if (version == SWITIFY_IPC_API_VERSION &&
      status.struct_size == sizeof(status) &&
      status.abi_version == SWITIFY_IPC_STATUS_ABI_VERSION &&
      (capabilities & SwitifyIpcCapability_Status))
    printf("\nHEALTH CHECK PASSED\n");
  else
    printf("\nHEALTH CHECK FAILED: incompatible service\n");
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  consoleInit(NULL);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  refresh();
  printf("\nA: launch/refresh    X: stop service    B: exit\n");
  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const uint64_t down = padGetButtonsDown(&pad);
    if (down & HidNpadButton_B) break;
    if (down & HidNpadButton_X) {
      stopService();
      printf("\nA: launch/refresh    X: stop service    B: exit\n");
      consoleUpdate(NULL);
      continue;
    }
    if (down & HidNpadButton_A) {
      bool ready = !service_missing;
      if (service_missing) ready = launchService();
      if (ready) refresh();
      printf("\nA: launch/refresh    X: stop service    B: exit\n");
      consoleUpdate(NULL);
    }
  }

  switifyIpcExit();
  consoleExit(NULL);
  return 0;
}
