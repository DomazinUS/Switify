#pragma once

#include "switify/ipc_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace switify::phase2 {

struct ControlMessage {
  SwitifyIpcCommand command = SwitifyIpcCommand_PlayPause;
  uint32_t value = 0;
  uint64_t sequence = 0;
  uint64_t generation = 0;
  std::array<char, SWITIFY_IPC_BROWSE_QUERY_SIZE> query{};
};

// Shared state and a bounded IPC-to-engine mailbox. The IPC thread only
// snapshots state and enqueues controls; Spotify, network, decoder, and audout
// work remains owned by the background engine thread.
class ServiceCore {
 public:
  static constexpr size_t CommandCapacity = 16;

  ServiceCore();

  SwitifyIpcStatus snapshot() const;
  uint64_t capabilities() const;
  SwitifyIpcBrowseHeader browseHeader() const;
  bool browseItem(uint32_t index, SwitifyIpcBrowseItem& item) const;

  void publishServiceState(SwitifyServiceState state, uint64_t monotonicMs);
  void publishPlaybackState(SwitifyPlaybackState state, uint32_t positionMs,
                            uint32_t durationMs, uint64_t monotonicMs);
  void publishTrack(std::string_view title, std::string_view artist,
                    std::string_view album, uint64_t monotonicMs);
  void clearTrack(uint64_t monotonicMs);
  void publishQueue(uint32_t index, uint32_t count, uint64_t monotonicMs);
  void publishVolume(uint32_t volumePermille, uint64_t monotonicMs);
  void publishConnection(bool connected, bool hasCredentials,
                         uint64_t monotonicMs);
  void publishShuffle(bool enabled, uint64_t monotonicMs);
  void publishRepeat(SwitifyRepeatMode mode, uint64_t monotonicMs);
  void publishAudioQuality(bool highQuality, uint64_t monotonicMs);
  void publishSeekable(bool seekable, uint64_t monotonicMs);
  void publishResult(SwitifyControlResult result, uint64_t monotonicMs);
  uint64_t beginBrowse(std::string_view title, std::string_view subtitle,
                       uint32_t depth, uint64_t monotonicMs);
  bool publishBrowse(uint64_t generation, std::string_view title,
                     std::string_view subtitle,
                     const SwitifyIpcBrowseItem* items, size_t itemCount,
                     uint32_t depth, uint64_t monotonicMs);
  bool publishBrowseError(uint64_t generation, std::string_view message,
                          uint64_t monotonicMs);

  void configureCommands(bool accepting, uint64_t capabilities,
                         uint64_t monotonicMs);
  SwitifyControlResult submit(SwitifyIpcCommand command, uint32_t value,
                              uint64_t monotonicMs);
  SwitifyControlResult submitBrowse(SwitifyIpcCommand command,
                                    const SwitifyIpcBrowseRequest& request,
                                    uint64_t monotonicMs);
  bool tryPop(ControlMessage& message);

 private:
  static bool isControlCommand(SwitifyIpcCommand command);
  static SwitifyControlResult validate(SwitifyIpcCommand command,
                                       uint32_t value);
  static void copyText(char* destination, size_t capacity,
                       std::string_view text);
  void changedLocked(uint64_t monotonicMs);
  void setFlagLocked(uint32_t flag, bool enabled);

  mutable std::mutex mutex;
  SwitifyIpcStatus status{};
  uint64_t advertisedCapabilities = SwitifyIpcCapability_Status;
  bool acceptingCommands = false;
  std::array<ControlMessage, CommandCapacity> commands{};
  size_t commandHead = 0;
  size_t commandCount = 0;
  uint64_t nextCommandSequence = 1;
  SwitifyIpcBrowseHeader browser{};
  std::array<SwitifyIpcBrowseItem, 32> browseItems{};
  uint64_t nextBrowseGeneration = 1;
};

}  // namespace switify::phase2
