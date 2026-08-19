#include "switify/service_core.hpp"

#include <algorithm>
#include <cstring>

namespace switify::phase2 {

ServiceCore::ServiceCore() {
  status.struct_size = sizeof(status);
  status.abi_version = SWITIFY_IPC_STATUS_ABI_VERSION;
  status.service_state = SwitifyServiceState_Booting;
  status.playback_state = SwitifyPlaybackState_Stopped;
  status.volume_permille = 800;
  status.queue_index = SWITIFY_IPC_INVALID_INDEX;
  status.last_result = SwitifyControlResult_BackendUnavailable;
  browser.struct_size = sizeof(browser);
  browser.abi_version = SWITIFY_IPC_BROWSE_ABI_VERSION;
  browser.state = SwitifyBrowseState_Idle;
}

SwitifyIpcStatus ServiceCore::snapshot() const {
  std::scoped_lock lock(mutex);
  return status;
}

uint64_t ServiceCore::capabilities() const {
  std::scoped_lock lock(mutex);
  return advertisedCapabilities;
}

SwitifyIpcBrowseHeader ServiceCore::browseHeader() const {
  std::scoped_lock lock(mutex);
  return browser;
}

bool ServiceCore::browseItem(uint32_t index, SwitifyIpcBrowseItem& item) const {
  std::scoped_lock lock(mutex);
  if (browser.state != SwitifyBrowseState_Ready ||
      index >= browser.item_count || index >= browseItems.size())
    return false;
  item = browseItems[index];
  return true;
}

void ServiceCore::changedLocked(uint64_t monotonicMs) {
  ++status.sequence;
  status.monotonic_ms = monotonicMs;
}

void ServiceCore::setFlagLocked(uint32_t flag, bool enabled) {
  if (enabled)
    status.flags |= flag;
  else
    status.flags &= ~flag;
}

void ServiceCore::publishServiceState(SwitifyServiceState state,
                                      uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  status.service_state = state;
  changedLocked(monotonicMs);
}

void ServiceCore::publishPlaybackState(SwitifyPlaybackState state,
                                       uint32_t positionMs,
                                       uint32_t durationMs,
                                       uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  status.playback_state = state;
  status.position_ms = positionMs;
  status.duration_ms = durationMs;
  changedLocked(monotonicMs);
}

void ServiceCore::copyText(char* destination, size_t capacity,
                           std::string_view text) {
  if (capacity == 0) return;
  const size_t length = std::min(capacity - 1, text.size());
  if (length != 0) std::memcpy(destination, text.data(), length);
  destination[length] = '\0';
  if (length + 1 < capacity)
    std::memset(destination + length + 1, 0, capacity - length - 1);
}

void ServiceCore::publishTrack(std::string_view title,
                               std::string_view artist,
                               std::string_view album,
                               uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  copyText(status.title, sizeof(status.title), title);
  copyText(status.artist, sizeof(status.artist), artist);
  copyText(status.album, sizeof(status.album), album);
  setFlagLocked(SwitifyStatusFlag_HasTrack, !title.empty());
  changedLocked(monotonicMs);
}

void ServiceCore::clearTrack(uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  std::memset(status.title, 0, sizeof(status.title));
  std::memset(status.artist, 0, sizeof(status.artist));
  std::memset(status.album, 0, sizeof(status.album));
  status.position_ms = 0;
  status.duration_ms = 0;
  status.playback_state = SwitifyPlaybackState_Stopped;
  setFlagLocked(SwitifyStatusFlag_HasTrack, false);
  setFlagLocked(SwitifyStatusFlag_CanSeek, false);
  changedLocked(monotonicMs);
}

void ServiceCore::publishQueue(uint32_t index, uint32_t count,
                               uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  status.queue_index = count == 0 ? SWITIFY_IPC_INVALID_INDEX : index;
  status.queue_count = count;
  changedLocked(monotonicMs);
}

void ServiceCore::publishVolume(uint32_t volumePermille,
                                uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  status.volume_permille = std::min(volumePermille, 1000u);
  changedLocked(monotonicMs);
}

void ServiceCore::publishConnection(bool connected, bool hasCredentials,
                                    uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  setFlagLocked(SwitifyStatusFlag_Connected, connected);
  setFlagLocked(SwitifyStatusFlag_HasCredentials, hasCredentials);
  changedLocked(monotonicMs);
}

void ServiceCore::publishShuffle(bool enabled, uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  setFlagLocked(SwitifyStatusFlag_Shuffle, enabled);
  changedLocked(monotonicMs);
}

void ServiceCore::publishRepeat(SwitifyRepeatMode mode,
                                uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  setFlagLocked(SwitifyStatusFlag_RepeatOne,
                mode == SwitifyRepeatMode_One);
  setFlagLocked(SwitifyStatusFlag_RepeatAll,
                mode == SwitifyRepeatMode_All);
  changedLocked(monotonicMs);
}

void ServiceCore::publishAudioQuality(bool highQuality,
                                      uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  setFlagLocked(SwitifyStatusFlag_HighQuality, highQuality);
  changedLocked(monotonicMs);
}

void ServiceCore::publishSeekable(bool seekable, uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  setFlagLocked(SwitifyStatusFlag_CanSeek, seekable);
  changedLocked(monotonicMs);
}

void ServiceCore::publishResult(SwitifyControlResult result,
                                uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  status.last_result = result;
  changedLocked(monotonicMs);
}

uint64_t ServiceCore::beginBrowse(std::string_view title,
                                  std::string_view subtitle, uint32_t depth,
                                  uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  const uint64_t generation = nextBrowseGeneration++;
  browser = {};
  browser.struct_size = sizeof(browser);
  browser.abi_version = SWITIFY_IPC_BROWSE_ABI_VERSION;
  browser.generation = generation;
  browser.state = SwitifyBrowseState_Loading;
  browser.depth = depth;
  copyText(browser.title, sizeof(browser.title), title);
  copyText(browser.subtitle, sizeof(browser.subtitle), subtitle);
  status.monotonic_ms = monotonicMs;
  return generation;
}

bool ServiceCore::publishBrowse(uint64_t generation, std::string_view title,
                                std::string_view subtitle,
                                const SwitifyIpcBrowseItem* items,
                                size_t itemCount, uint32_t depth,
                                uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  if (browser.generation != generation) return false;
  const size_t count = std::min(itemCount, browseItems.size());
  for (size_t index = 0; index < count; ++index) {
    browseItems[index] = items[index];
    browseItems[index].struct_size = sizeof(SwitifyIpcBrowseItem);
    browseItems[index].abi_version = SWITIFY_IPC_BROWSE_ABI_VERSION;
    browseItems[index].generation = generation;
    browseItems[index].index = static_cast<uint32_t>(index);
  }
  browser.state = SwitifyBrowseState_Ready;
  browser.depth = depth;
  browser.item_count = static_cast<uint32_t>(count);
  browser.message[0] = '\0';
  copyText(browser.title, sizeof(browser.title), title);
  copyText(browser.subtitle, sizeof(browser.subtitle), subtitle);
  status.monotonic_ms = monotonicMs;
  return true;
}

bool ServiceCore::publishBrowseError(uint64_t generation,
                                     std::string_view message,
                                     uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  if (browser.generation != generation) return false;
  browser.state = SwitifyBrowseState_Error;
  browser.item_count = 0;
  copyText(browser.message, sizeof(browser.message), message);
  status.monotonic_ms = monotonicMs;
  return true;
}

void ServiceCore::configureCommands(bool accepting, uint64_t capabilities,
                                    uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  acceptingCommands = accepting;
  advertisedCapabilities = capabilities | SwitifyIpcCapability_Status;
  if (!accepting) {
    commandHead = 0;
    commandCount = 0;
  }
  changedLocked(monotonicMs);
}

bool ServiceCore::isControlCommand(SwitifyIpcCommand command) {
  return (command >= SwitifyIpcCommand_PlayPause &&
          command <= SwitifyIpcCommand_SetAudioQuality) ||
         (command >= SwitifyIpcCommand_BrowseHome &&
          command <= SwitifyIpcCommand_BrowseQueue);
}

SwitifyControlResult ServiceCore::validate(SwitifyIpcCommand command,
                                           uint32_t value) {
  if (!isControlCommand(command))
    return SwitifyControlResult_InvalidCommand;
  if (command == SwitifyIpcCommand_SetVolume && value > 1000)
    return SwitifyControlResult_InvalidArgument;
  if (command == SwitifyIpcCommand_SetShuffle && value > 1)
    return SwitifyControlResult_InvalidArgument;
  if (command == SwitifyIpcCommand_SetRepeatMode &&
      value > SwitifyRepeatMode_All)
    return SwitifyControlResult_InvalidArgument;
  if (command == SwitifyIpcCommand_SetAudioQuality && value > 1)
    return SwitifyControlResult_InvalidArgument;
  return SwitifyControlResult_Ok;
}

SwitifyControlResult ServiceCore::submit(SwitifyIpcCommand command,
                                         uint32_t value,
                                         uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  SwitifyControlResult result = validate(command, value);
  if (result == SwitifyControlResult_Ok && !acceptingCommands)
    result = SwitifyControlResult_BackendUnavailable;

  // State-setting controls are idempotent. If the UI updates them faster than
  // the engine thread consumes its mailbox, retain only the newest requested
  // state. This prevents stale Play/Pause, seek, volume, Shuffle, and Repeat
  // commands from being replayed after the UI has already moved on.
  if (result == SwitifyControlResult_Ok) {
    const bool replaceable =
        command == SwitifyIpcCommand_Play ||
        command == SwitifyIpcCommand_Pause ||
        command == SwitifyIpcCommand_SetVolume ||
        command == SwitifyIpcCommand_Seek ||
        command == SwitifyIpcCommand_SetShuffle ||
        command == SwitifyIpcCommand_SetRepeatMode ||
        command == SwitifyIpcCommand_SetAudioQuality;
    if (replaceable) {
      for (size_t offset = 0; offset < commandCount; ++offset) {
        const size_t index = (commandHead + offset) % commands.size();
        const auto queued = commands[index].command;
        const bool sameStateFamily =
            queued == command ||
            ((command == SwitifyIpcCommand_Play ||
              command == SwitifyIpcCommand_Pause) &&
             (queued == SwitifyIpcCommand_Play ||
              queued == SwitifyIpcCommand_Pause));
        if (!sameStateFamily) continue;
        commands[index].command = command;
        commands[index].value = value;
        commands[index].sequence = nextCommandSequence++;
        status.last_result = SwitifyControlResult_Ok;
        changedLocked(monotonicMs);
        return SwitifyControlResult_Ok;
      }
    }
  }
  if (result == SwitifyControlResult_Ok &&
      commandCount == commands.size())
    result = SwitifyControlResult_Busy;

  if (result == SwitifyControlResult_Ok) {
    const size_t tail = (commandHead + commandCount) % commands.size();
    commands[tail] = {};
    commands[tail].command = command;
    commands[tail].value = value;
    commands[tail].sequence = nextCommandSequence++;
    ++commandCount;
  }

  status.last_result = result;
  changedLocked(monotonicMs);
  return result;
}

SwitifyControlResult ServiceCore::submitBrowse(
    SwitifyIpcCommand command, const SwitifyIpcBrowseRequest& request,
    uint64_t monotonicMs) {
  std::scoped_lock lock(mutex);
  SwitifyControlResult result = validate(command, request.index);
  if (result == SwitifyControlResult_Ok && !acceptingCommands)
    result = SwitifyControlResult_BackendUnavailable;
  if (result == SwitifyControlResult_Ok && commandCount == commands.size())
    result = SwitifyControlResult_Busy;
  if (result == SwitifyControlResult_Ok) {
    const size_t tail = (commandHead + commandCount) % commands.size();
    commands[tail] = {};
    commands[tail].command = command;
    commands[tail].value = request.index;
    commands[tail].generation = request.generation;
    const char* queryEnd =
        std::find(request.query, request.query + sizeof(request.query), '\0');
    copyText(commands[tail].query.data(), commands[tail].query.size(),
             std::string_view(request.query,
                              static_cast<size_t>(queryEnd - request.query)));
    commands[tail].sequence = nextCommandSequence++;
    ++commandCount;
  }
  status.last_result = result;
  changedLocked(monotonicMs);
  return result;
}

bool ServiceCore::tryPop(ControlMessage& message) {
  std::scoped_lock lock(mutex);
  if (commandCount == 0) return false;
  message = commands[commandHead];
  commandHead = (commandHead + 1) % commands.size();
  --commandCount;
  return true;
}

}  // namespace switify::phase2
