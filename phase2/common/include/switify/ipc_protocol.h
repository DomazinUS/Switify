#pragma once

// Stable C ABI between the Switify background sysmodule and its independent
// overlay (plus developer diagnostics). The standalone NRO does not depend on
// this runtime connection.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITIFY_IPC_SERVICE_NAME "switify"

#define SWITIFY_IPC_API_VERSION_MAJOR 1u
#define SWITIFY_IPC_API_VERSION_MINOR 2u
#define SWITIFY_IPC_API_VERSION \
    ((SWITIFY_IPC_API_VERSION_MAJOR << 16u) | SWITIFY_IPC_API_VERSION_MINOR)

#define SWITIFY_IPC_STATUS_ABI_VERSION 1u
#define SWITIFY_IPC_STATUS_SIZE 216u
#define SWITIFY_IPC_INVALID_INDEX UINT32_MAX

typedef enum SwitifyIpcCommand {
    SwitifyIpcCommand_GetApiVersion = 0,
    SwitifyIpcCommand_GetCapabilities = 1,
    SwitifyIpcCommand_GetStatus = 2,
    SwitifyIpcCommand_Ping = 3,

    SwitifyIpcCommand_PlayPause = 10,
    SwitifyIpcCommand_Play = 11,
    SwitifyIpcCommand_Pause = 12,
    SwitifyIpcCommand_Next = 13,
    SwitifyIpcCommand_Previous = 14,
    SwitifyIpcCommand_SetVolume = 15,
    SwitifyIpcCommand_Seek = 16,
    SwitifyIpcCommand_SetShuffle = 17,
    SwitifyIpcCommand_SetRepeatMode = 18,
    SwitifyIpcCommand_SetAudioQuality = 19,

    SwitifyIpcCommand_GetBrowseHeader = 20,
    SwitifyIpcCommand_GetBrowseItem = 21,

    SwitifyIpcCommand_BrowseHome = 30,
    SwitifyIpcCommand_BrowseLibrary = 31,
    SwitifyIpcCommand_BrowseSearch = 32,
    SwitifyIpcCommand_BrowseOpen = 33,
    SwitifyIpcCommand_BrowseBack = 34,
    SwitifyIpcCommand_BrowsePlay = 35,
    SwitifyIpcCommand_BrowseCurrentPlaylist = 36,
    SwitifyIpcCommand_BrowseQueue = 37,
} SwitifyIpcCommand;

typedef enum SwitifyIpcCapability {
    SwitifyIpcCapability_Status = UINT64_C(1) << 0u,
    SwitifyIpcCapability_PlaybackControl = UINT64_C(1) << 1u,
    SwitifyIpcCapability_Seek = UINT64_C(1) << 2u,
    SwitifyIpcCapability_Queue = UINT64_C(1) << 3u,
    SwitifyIpcCapability_Authentication = UINT64_C(1) << 4u,
    SwitifyIpcCapability_Browsing = UINT64_C(1) << 5u,
    SwitifyIpcCapability_Search = UINT64_C(1) << 6u,
    SwitifyIpcCapability_AudioSettings = UINT64_C(1) << 7u,
} SwitifyIpcCapability;

typedef enum SwitifyServiceState {
    SwitifyServiceState_Booting = 0,
    SwitifyServiceState_WaitingForCredentials = 1,
    SwitifyServiceState_Pairing = 2,
    SwitifyServiceState_Connecting = 3,
    SwitifyServiceState_Ready = 4,
    SwitifyServiceState_Error = 5,
    SwitifyServiceState_Stopping = 6,
} SwitifyServiceState;

typedef enum SwitifyPlaybackState {
    SwitifyPlaybackState_Stopped = 0,
    SwitifyPlaybackState_Buffering = 1,
    SwitifyPlaybackState_Playing = 2,
    SwitifyPlaybackState_Paused = 3,
} SwitifyPlaybackState;

typedef enum SwitifyRepeatMode {
    SwitifyRepeatMode_Off = 0,
    SwitifyRepeatMode_One = 1,
    SwitifyRepeatMode_All = 2,
} SwitifyRepeatMode;

typedef enum SwitifyStatusFlag {
    SwitifyStatusFlag_Connected = 1u << 0u,
    SwitifyStatusFlag_HasCredentials = 1u << 1u,
    SwitifyStatusFlag_Shuffle = 1u << 2u,
    SwitifyStatusFlag_RepeatOne = 1u << 3u,
    SwitifyStatusFlag_RepeatAll = 1u << 4u,
    SwitifyStatusFlag_CanSeek = 1u << 5u,
    SwitifyStatusFlag_HasTrack = 1u << 6u,
    SwitifyStatusFlag_HighQuality = 1u << 7u,
} SwitifyStatusFlag;

typedef enum SwitifyControlResult {
    SwitifyControlResult_Ok = 0,
    SwitifyControlResult_InvalidCommand = 1,
    SwitifyControlResult_InvalidArgument = 2,
    SwitifyControlResult_BackendUnavailable = 3,
    SwitifyControlResult_Busy = 4,
} SwitifyControlResult;

typedef enum SwitifyBrowseState {
    SwitifyBrowseState_Idle = 0,
    SwitifyBrowseState_Loading = 1,
    SwitifyBrowseState_Ready = 2,
    SwitifyBrowseState_Error = 3,
} SwitifyBrowseState;

typedef enum SwitifyBrowseItemKind {
    SwitifyBrowseItemKind_Section = 0,
    SwitifyBrowseItemKind_Track = 1,
    SwitifyBrowseItemKind_Playlist = 2,
    SwitifyBrowseItemKind_Album = 3,
    SwitifyBrowseItemKind_Artist = 4,
} SwitifyBrowseItemKind;

#define SWITIFY_IPC_BROWSE_ABI_VERSION 1u
#define SWITIFY_IPC_BROWSE_HEADER_SIZE 216u
#define SWITIFY_IPC_BROWSE_ITEM_SIZE 216u
#define SWITIFY_IPC_BROWSE_QUERY_SIZE 128u

typedef struct SwitifyIpcBrowseHeader {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t generation;
    uint32_t state;
    uint32_t depth;
    uint32_t item_count;
    uint32_t flags;
    char title[64];
    char subtitle[64];
    char message[56];
} SwitifyIpcBrowseHeader;

typedef struct SwitifyIpcBrowseItem {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t generation;
    uint32_t kind;
    uint32_t index;
    uint32_t duration_ms;
    uint32_t flags;
    char name[80];
    char subtitle[64];
    char detail[40];
} SwitifyIpcBrowseItem;

typedef struct SwitifyIpcBrowseRequest {
    uint64_t generation;
    uint32_t index;
    uint32_t reserved;
    char query[SWITIFY_IPC_BROWSE_QUERY_SIZE];
} SwitifyIpcBrowseRequest;

// Exactly one TLS-backed inline CMIF response (216 bytes). Adding fields
// requires a new
// status ABI version; old fields must retain their offsets.
typedef struct SwitifyIpcStatus {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t sequence;
    uint64_t monotonic_ms;
    uint32_t service_state;
    uint32_t playback_state;
    uint32_t flags;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t volume_permille;
    uint32_t queue_index;
    uint32_t queue_count;
    uint32_t last_result;
    uint32_t reserved0;
    char title[64];
    char artist[48];
    char album[40];
} SwitifyIpcStatus;

#ifdef __cplusplus
}

static_assert(sizeof(SwitifyIpcStatus) == SWITIFY_IPC_STATUS_SIZE,
              "Switify IPC status ABI changed");
static_assert(sizeof(SwitifyIpcBrowseHeader) == SWITIFY_IPC_BROWSE_HEADER_SIZE,
              "Switify browse header ABI changed");
static_assert(sizeof(SwitifyIpcBrowseItem) == SWITIFY_IPC_BROWSE_ITEM_SIZE,
              "Switify browse item ABI changed");
static_assert(sizeof(SwitifyIpcBrowseRequest) == 144,
              "Switify browse request ABI changed");
#endif
