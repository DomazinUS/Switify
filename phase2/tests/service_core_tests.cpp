#include "switify/service_core.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

using switify::phase2::ControlMessage;
using switify::phase2::ServiceCore;

int main() {
  static_assert(sizeof(SwitifyIpcStatus) == SWITIFY_IPC_STATUS_SIZE);
  static_assert(sizeof(SwitifyIpcBrowseHeader) ==
                SWITIFY_IPC_BROWSE_HEADER_SIZE);
  static_assert(sizeof(SwitifyIpcBrowseItem) ==
                SWITIFY_IPC_BROWSE_ITEM_SIZE);

  ServiceCore core;
  auto status = core.snapshot();
  assert(status.struct_size == SWITIFY_IPC_STATUS_SIZE);
  assert(status.abi_version == SWITIFY_IPC_STATUS_ABI_VERSION);
  assert(status.service_state == SwitifyServiceState_Booting);
  assert(status.playback_state == SwitifyPlaybackState_Stopped);
  assert(status.volume_permille == 800);
  assert(status.queue_index == SWITIFY_IPC_INVALID_INDEX);
  assert(core.capabilities() == SwitifyIpcCapability_Status);

  const std::string longTitle(100, 'T');
  core.publishTrack(longTitle, "Artist", "Album", 100);
  core.publishPlaybackState(SwitifyPlaybackState_Playing, 1234, 9876, 101);
  core.publishConnection(true, true, 102);
  core.publishQueue(2, 12, 103);
  status = core.snapshot();
  assert(std::strlen(status.title) == sizeof(status.title) - 1);
  assert(std::strcmp(status.artist, "Artist") == 0);
  assert(status.flags & SwitifyStatusFlag_HasTrack);
  assert(status.flags & SwitifyStatusFlag_Connected);
  assert(status.flags & SwitifyStatusFlag_HasCredentials);
  assert(status.position_ms == 1234);
  assert(status.duration_ms == 9876);
  assert(status.queue_index == 2);
  assert(status.queue_count == 12);
  assert(status.monotonic_ms == 103);

  assert(core.submit(SwitifyIpcCommand_Play, 0, 104) ==
         SwitifyControlResult_BackendUnavailable);

  const uint64_t capabilities = SwitifyIpcCapability_PlaybackControl |
                                SwitifyIpcCapability_Seek;
  core.configureCommands(true, capabilities, 105);
  assert(core.capabilities() ==
         (capabilities | SwitifyIpcCapability_Status));
  assert(core.submit(SwitifyIpcCommand_Play, 0, 106) ==
         SwitifyControlResult_Ok);
  assert(core.submit(SwitifyIpcCommand_SetVolume, 750, 107) ==
         SwitifyControlResult_Ok);
  assert(core.submit(SwitifyIpcCommand_SetVolume, 1001, 108) ==
         SwitifyControlResult_InvalidArgument);
  assert(core.submit(SwitifyIpcCommand_SetRepeatMode,
                     SwitifyRepeatMode_One, 108) ==
         SwitifyControlResult_Ok);
  assert(core.submit(SwitifyIpcCommand_SetAudioQuality, 1, 108) ==
         SwitifyControlResult_Ok);
  assert(core.submit(SwitifyIpcCommand_SetAudioQuality, 2, 108) ==
         SwitifyControlResult_InvalidArgument);

  ControlMessage message;
  assert(core.tryPop(message));
  assert(message.command == SwitifyIpcCommand_Play);
  assert(message.sequence == 1);
  assert(core.tryPop(message));
  assert(message.command == SwitifyIpcCommand_SetVolume);
  assert(message.value == 750);
  assert(message.sequence == 2);
  assert(core.tryPop(message));
  assert(message.command == SwitifyIpcCommand_SetRepeatMode);
  assert(message.value == SwitifyRepeatMode_One);
  assert(message.sequence == 3);
  assert(core.tryPop(message));
  assert(message.command == SwitifyIpcCommand_SetAudioQuality);
  assert(message.value == 1);
  assert(message.sequence == 4);
  assert(!core.tryPop(message));

  // Explicit state controls are coalesced while the engine thread is busy.
  // Only the newest requested state may be replayed later.
  assert(core.submit(SwitifyIpcCommand_Play, 0, 109) ==
         SwitifyControlResult_Ok);
  assert(core.submit(SwitifyIpcCommand_Pause, 0, 110) ==
         SwitifyControlResult_Ok);
  assert(core.submit(SwitifyIpcCommand_SetVolume, 200, 111) ==
         SwitifyControlResult_Ok);
  assert(core.submit(SwitifyIpcCommand_SetVolume, 650, 112) ==
         SwitifyControlResult_Ok);
  assert(core.tryPop(message));
  assert(message.command == SwitifyIpcCommand_Pause);
  assert(core.tryPop(message));
  assert(message.command == SwitifyIpcCommand_SetVolume);
  assert(message.value == 650);
  assert(!core.tryPop(message));

  core.publishRepeat(SwitifyRepeatMode_One, 108);
  status = core.snapshot();
  assert(status.flags & SwitifyStatusFlag_RepeatOne);
  assert(!(status.flags & SwitifyStatusFlag_RepeatAll));
  core.publishRepeat(SwitifyRepeatMode_All, 108);
  status = core.snapshot();
  assert(!(status.flags & SwitifyStatusFlag_RepeatOne));
  assert(status.flags & SwitifyStatusFlag_RepeatAll);
  core.publishAudioQuality(true, 108);
  status = core.snapshot();
  assert(status.flags & SwitifyStatusFlag_HighQuality);

  const uint64_t browseGeneration =
      core.beginBrowse("Home", "For you", 0, 109);
  auto browseHeader = core.browseHeader();
  assert(browseHeader.generation == browseGeneration);
  assert(browseHeader.state == SwitifyBrowseState_Loading);
  assert(std::strcmp(browseHeader.title, "Home") == 0);

  SwitifyIpcBrowseItem browseItems[2]{};
  browseItems[0].kind = SwitifyBrowseItemKind_Section;
  std::strcpy(browseItems[0].name, "Recently played");
  browseItems[1].kind = SwitifyBrowseItemKind_Track;
  browseItems[1].duration_ms = 123000;
  std::strcpy(browseItems[1].name, "Test track");
  assert(core.publishBrowse(browseGeneration, "Home", "For you",
                            browseItems, 2, 0, 110));
  browseHeader = core.browseHeader();
  assert(browseHeader.state == SwitifyBrowseState_Ready);
  assert(browseHeader.item_count == 2);
  SwitifyIpcBrowseItem browseItem{};
  assert(core.browseItem(1, browseItem));
  assert(browseItem.generation == browseGeneration);
  assert(browseItem.index == 1);
  assert(std::strcmp(browseItem.name, "Test track") == 0);
  assert(!core.browseItem(2, browseItem));

  SwitifyIpcBrowseRequest browseRequest{};
  browseRequest.generation = browseGeneration;
  browseRequest.index = 1;
  std::strcpy(browseRequest.query, "metallica");
  assert(core.submitBrowse(SwitifyIpcCommand_BrowseOpen, browseRequest, 111) ==
         SwitifyControlResult_Ok);
  assert(core.tryPop(message));
  assert(message.command == SwitifyIpcCommand_BrowseOpen);
  assert(message.generation == browseGeneration);
  assert(message.value == 1);
  assert(std::strcmp(message.query.data(), "metallica") == 0);

  const uint64_t failedGeneration =
      core.beginBrowse("Search", "query", 0, 112);
  assert(core.publishBrowseError(failedGeneration, "Network unavailable",
                                 113));
  browseHeader = core.browseHeader();
  assert(browseHeader.state == SwitifyBrowseState_Error);
  assert(std::strcmp(browseHeader.message, "Network unavailable") == 0);

  for (size_t i = 0; i < ServiceCore::CommandCapacity; ++i) {
    assert(core.submit(SwitifyIpcCommand_Next, 0, 200 + i) ==
           SwitifyControlResult_Ok);
  }
  assert(core.submit(SwitifyIpcCommand_Next, 0, 300) ==
         SwitifyControlResult_Busy);

  core.clearTrack(400);
  status = core.snapshot();
  assert(!(status.flags & SwitifyStatusFlag_HasTrack));
  assert(status.title[0] == '\0');
  assert(status.playback_state == SwitifyPlaybackState_Stopped);

  std::cout << "Switify Phase 2 service-core tests passed\n";
  return 0;
}
