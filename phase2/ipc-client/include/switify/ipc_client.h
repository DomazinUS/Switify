#pragma once

#include "switify/ipc_protocol.h"

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

Result switifyIpcInitialize(void);
void switifyIpcExit(void);
bool switifyIpcIsActive(void);

Result switifyIpcPing(void);
Result switifyIpcGetApiVersion(uint32_t* version);
Result switifyIpcGetCapabilities(uint64_t* capabilities);
Result switifyIpcGetStatus(SwitifyIpcStatus* status);

Result switifyIpcPlayPause(void);
Result switifyIpcPlay(void);
Result switifyIpcPause(void);
Result switifyIpcNext(void);
Result switifyIpcPrevious(void);
Result switifyIpcSetVolume(uint32_t volumePermille);
Result switifyIpcSeek(uint32_t positionMs);
Result switifyIpcSetShuffle(bool enabled);
Result switifyIpcSetRepeatMode(SwitifyRepeatMode mode);
Result switifyIpcSetAudioQuality(bool highQuality);

Result switifyIpcGetBrowseHeader(SwitifyIpcBrowseHeader* header);
Result switifyIpcGetBrowseItem(uint32_t index, SwitifyIpcBrowseItem* item);
Result switifyIpcBrowseHome(void);
Result switifyIpcBrowseLibrary(void);
Result switifyIpcBrowseSearch(const char* query);
Result switifyIpcBrowseOpen(uint64_t generation, uint32_t index);
Result switifyIpcBrowseBack(uint64_t generation);
Result switifyIpcBrowsePlay(uint64_t generation, uint32_t index);
Result switifyIpcBrowseCurrentPlaylist(void);
Result switifyIpcBrowseQueue(void);

#ifdef __cplusplus
}
#endif
