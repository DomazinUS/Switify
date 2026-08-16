#include "switify_service.hpp"

#include "ipc_server.h"
#include "switify/ipc_protocol.h"

#include <cstring>

namespace switify::phase2::switch_service {
namespace {

constexpr uint32_t ResultModule = 421;
SwitifyIpcServer server{};
ServiceCore* serviceCore = nullptr;

uint64_t monotonicMs() {
  return armTicksToNs(armGetSystemTick()) / UINT64_C(1000000);
}

Result controlResult(SwitifyControlResult result) {
  if (result == SwitifyControlResult_Ok) return 0;
  return MAKERESULT(ResultModule, (uint32_t)result);
}

template <typename T>
Result writeOutput(uint8_t* output, size_t* outputSize, const T& value) {
  if (sizeof(T) > SWITIFY_IPC_RESPONSE_MAX_DATA_SIZE)
    return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  std::memcpy(output, &value, sizeof(value));
  *outputSize = sizeof(value);
  return 0;
}

bool readU32(const SwitifyIpcServerRequest* request, uint32_t& value) {
  if (request->data_size < sizeof(value) || !request->data) return false;
  std::memcpy(&value, request->data, sizeof(value));
  return true;
}

bool readBrowseRequest(const SwitifyIpcServerRequest* request,
                       SwitifyIpcBrowseRequest& value) {
  if (request->data_size < sizeof(value) || !request->data) return false;
  std::memcpy(&value, request->data, sizeof(value));
  value.query[sizeof(value.query) - 1] = '\0';
  return true;
}

Result handle(void*, const SwitifyIpcServerRequest* request, uint8_t* output,
              size_t* outputSize) {
  const auto command = static_cast<SwitifyIpcCommand>(request->command_id);
  switch (command) {
    case SwitifyIpcCommand_Ping:
      return 0;
    case SwitifyIpcCommand_GetApiVersion: {
      const uint32_t version = SWITIFY_IPC_API_VERSION;
      return writeOutput(output, outputSize, version);
    }
    case SwitifyIpcCommand_GetCapabilities: {
      const uint64_t capabilities = serviceCore->capabilities();
      return writeOutput(output, outputSize, capabilities);
    }
    case SwitifyIpcCommand_GetStatus: {
      const SwitifyIpcStatus status = serviceCore->snapshot();
      return writeOutput(output, outputSize, status);
    }
    case SwitifyIpcCommand_GetBrowseHeader: {
      const SwitifyIpcBrowseHeader header = serviceCore->browseHeader();
      return writeOutput(output, outputSize, header);
    }
    case SwitifyIpcCommand_GetBrowseItem: {
      uint32_t index = 0;
      SwitifyIpcBrowseItem item{};
      if (!readU32(request, index) || !serviceCore->browseItem(index, item))
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
      return writeOutput(output, outputSize, item);
    }
    default:
      break;
  }

  if (command >= SwitifyIpcCommand_BrowseHome &&
      command <= SwitifyIpcCommand_BrowseQueue) {
    SwitifyIpcBrowseRequest browseRequest{};
    if (!readBrowseRequest(request, browseRequest))
      return controlResult(SwitifyControlResult_InvalidArgument);
    return controlResult(
        serviceCore->submitBrowse(command, browseRequest, monotonicMs()));
  }

  uint32_t value = 0;
  if (command == SwitifyIpcCommand_SetVolume ||
      command == SwitifyIpcCommand_Seek ||
      command == SwitifyIpcCommand_SetShuffle ||
      command == SwitifyIpcCommand_SetRepeatMode ||
      command == SwitifyIpcCommand_SetAudioQuality) {
    if (!readU32(request, value))
      return controlResult(SwitifyControlResult_InvalidArgument);
  }
  return controlResult(serviceCore->submit(command, value, monotonicMs()));
}

}  // namespace

Result initialize(ServiceCore& core) {
  serviceCore = &core;
  return switifyIpcServerInitialize(&server, SWITIFY_IPC_SERVICE_NAME, 4);
}

Result exit() {
  serviceCore = nullptr;
  return switifyIpcServerExit(&server);
}

void loop() {
  while (true) {
    const Result result = switifyIpcServerProcess(&server, handle, nullptr);
    if (result == KERNELRESULT(Cancelled)) break;
  }
}

}  // namespace switify::phase2::switch_service
