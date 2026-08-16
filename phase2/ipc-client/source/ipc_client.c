#include "switify/ipc_client.h"

#include <string.h>

static Service g_switifyService;

Result switifyIpcInitialize(void) {
  if (serviceIsActive(&g_switifyService)) return 0;
  return smGetService(&g_switifyService, SWITIFY_IPC_SERVICE_NAME);
}

void switifyIpcExit(void) { serviceClose(&g_switifyService); }

bool switifyIpcIsActive(void) { return serviceIsActive(&g_switifyService); }

Result switifyIpcPing(void) {
  return serviceDispatch(&g_switifyService, SwitifyIpcCommand_Ping);
}

Result switifyIpcGetApiVersion(uint32_t* version) {
  if (!version) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  return serviceDispatchOut(&g_switifyService,
                            SwitifyIpcCommand_GetApiVersion, *version);
}

Result switifyIpcGetCapabilities(uint64_t* capabilities) {
  if (!capabilities) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  return serviceDispatchOut(&g_switifyService,
                            SwitifyIpcCommand_GetCapabilities, *capabilities);
}

Result switifyIpcGetStatus(SwitifyIpcStatus* status) {
  if (!status) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  memset(status, 0, sizeof(*status));
  return serviceDispatchOut(&g_switifyService, SwitifyIpcCommand_GetStatus,
                            *status);
}

static Result dispatchNoInput(SwitifyIpcCommand command) {
  return serviceDispatch(&g_switifyService, command);
}

static Result dispatchU32(SwitifyIpcCommand command, uint32_t value) {
  return serviceDispatchIn(&g_switifyService, command, value);
}

Result switifyIpcPlayPause(void) {
  return dispatchNoInput(SwitifyIpcCommand_PlayPause);
}

Result switifyIpcPlay(void) {
  return dispatchNoInput(SwitifyIpcCommand_Play);
}

Result switifyIpcPause(void) {
  return dispatchNoInput(SwitifyIpcCommand_Pause);
}

Result switifyIpcNext(void) {
  return dispatchNoInput(SwitifyIpcCommand_Next);
}

Result switifyIpcPrevious(void) {
  return dispatchNoInput(SwitifyIpcCommand_Previous);
}

Result switifyIpcSetVolume(uint32_t volumePermille) {
  return dispatchU32(SwitifyIpcCommand_SetVolume, volumePermille);
}

Result switifyIpcSeek(uint32_t positionMs) {
  return dispatchU32(SwitifyIpcCommand_Seek, positionMs);
}

Result switifyIpcSetShuffle(bool enabled) {
  return dispatchU32(SwitifyIpcCommand_SetShuffle, enabled ? 1u : 0u);
}

Result switifyIpcSetRepeatMode(SwitifyRepeatMode mode) {
  return dispatchU32(SwitifyIpcCommand_SetRepeatMode, (uint32_t)mode);
}

Result switifyIpcSetAudioQuality(bool highQuality) {
  return dispatchU32(SwitifyIpcCommand_SetAudioQuality,
                     highQuality ? 1u : 0u);
}

Result switifyIpcGetBrowseHeader(SwitifyIpcBrowseHeader* header) {
  if (!header) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  memset(header, 0, sizeof(*header));
  return serviceDispatchOut(&g_switifyService,
                            SwitifyIpcCommand_GetBrowseHeader, *header);
}

Result switifyIpcGetBrowseItem(uint32_t index, SwitifyIpcBrowseItem* item) {
  if (!item) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  memset(item, 0, sizeof(*item));
  return serviceDispatchInOut(&g_switifyService,
                              SwitifyIpcCommand_GetBrowseItem, index, *item);
}

static Result dispatchBrowseRequest(SwitifyIpcCommand command,
                                    uint64_t generation, uint32_t index,
                                    const char* query) {
  SwitifyIpcBrowseRequest request = {0};
  request.generation = generation;
  request.index = index;
  if (query) strncpy(request.query, query, sizeof(request.query) - 1);
  return serviceDispatchIn(&g_switifyService, command, request);
}

Result switifyIpcBrowseHome(void) {
  return dispatchBrowseRequest(SwitifyIpcCommand_BrowseHome, 0, 0, NULL);
}

Result switifyIpcBrowseLibrary(void) {
  return dispatchBrowseRequest(SwitifyIpcCommand_BrowseLibrary, 0, 0, NULL);
}

Result switifyIpcBrowseSearch(const char* query) {
  if (!query || !query[0])
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  return dispatchBrowseRequest(SwitifyIpcCommand_BrowseSearch, 0, 0, query);
}

Result switifyIpcBrowseOpen(uint64_t generation, uint32_t index) {
  return dispatchBrowseRequest(SwitifyIpcCommand_BrowseOpen, generation, index,
                               NULL);
}

Result switifyIpcBrowseBack(uint64_t generation) {
  return dispatchBrowseRequest(SwitifyIpcCommand_BrowseBack, generation, 0,
                               NULL);
}

Result switifyIpcBrowsePlay(uint64_t generation, uint32_t index) {
  return dispatchBrowseRequest(SwitifyIpcCommand_BrowsePlay, generation, index,
                               NULL);
}

Result switifyIpcBrowseCurrentPlaylist(void) {
  return dispatchBrowseRequest(SwitifyIpcCommand_BrowseCurrentPlaylist, 0, 0,
                               NULL);
}

Result switifyIpcBrowseQueue(void) {
  return dispatchBrowseRequest(SwitifyIpcCommand_BrowseQueue, 0, 0, NULL);
}
