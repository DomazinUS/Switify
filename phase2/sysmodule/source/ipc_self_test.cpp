#include "ipc_self_test.hpp"

#include "switify/ipc_protocol.h"

#include <cstdio>

namespace switify::phase2::ipc_self_test {
namespace {

constexpr const char* ResultPath =
    "sdmc:/config/Switify/background-ready.log";

alignas(0x1000) uint8_t threadStack[0x4000];
Thread thread{};

void writeResult(const char* stage, Result result) {
  uint64_t processId = 0;
  const Result processResult = svcGetProcessId(&processId, CUR_PROCESS_HANDLE);
  if (R_FAILED(processResult)) processId = 0;

  FILE* file = std::fopen(ResultPath, "w");
  if (!file) return;
  std::fprintf(file, "%s %016llX 0x%08X (M%u D%u)\n", stage,
               static_cast<unsigned long long>(processId), result,
               R_MODULE(result), R_DESCRIPTION(result));
  std::fclose(file);
}

void run(void*) {
  Service service{};
  Result result = smGetService(&service, SWITIFY_IPC_SERVICE_NAME);
  if (R_FAILED(result)) {
    writeResult("sm-get", result);
    return;
  }

  result = serviceDispatch(&service, SwitifyIpcCommand_Ping);
  serviceClose(&service);
  writeResult(R_SUCCEEDED(result) ? "pass-v043" : "ping-v043", result);
}

}  // namespace

Result start() {
  writeResult("pending", 0);
  Result result = threadCreate(&thread, run, nullptr, threadStack,
                               sizeof(threadStack), 49, 3);
  if (R_SUCCEEDED(result)) result = threadStart(&thread);
  if (R_FAILED(result)) writeResult("thread", result);
  return result;
}

}  // namespace switify::phase2::ipc_self_test
