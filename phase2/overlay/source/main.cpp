// SPDX-License-Identifier: GPL-2.0-only

#define TESLA_INIT_IMPL

#include "artwork_loader.hpp"
#include "switify/ipc_client.h"

#include <tesla.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

#ifdef SWITIFY_LIGHTWEIGHT_OVERLAY
constexpr const char* OverlayVersion = "v0.5.3-lite1";
#else
constexpr const char* OverlayVersion = "v0.5.3";
#endif

constexpr uint64_t SwitifyProgramId = UINT64_C(0x4200000000535754);
constexpr const char* ReadyPath =
    "sdmc:/config/Switify/background-ready.log";
constexpr const char* ArtworkUrlPath =
    "sdmc:/config/Switify/current-artwork-url";
constexpr auto RefreshInterval = std::chrono::milliseconds(500);
constexpr auto BrowseRefreshInterval = std::chrono::milliseconds(200);
constexpr auto InputClaimInterval = std::chrono::milliseconds(200);
constexpr unsigned AutomaticLaunchAttempts = 4;
constexpr auto StartupReadinessTimeout = std::chrono::seconds(6);
constexpr auto StartupRelaunchDelay = std::chrono::milliseconds(750);

enum class ConnectionState {
  Dormant,
  Starting,
  Connected,
  Incompatible,
  Failed,
};

ConnectionState connectionState = ConnectionState::Dormant;
SwitifyIpcStatus currentStatus{};
uint64_t capabilities = 0;
uint64_t runningPid = 0;
uint64_t connectedPid = 0;
std::string serviceText = "Dormant";
std::string feedbackText = "Starts automatically with Switify";
std::string readyText = "Not started";
bool automaticLaunchActive = false;
unsigned automaticLaunchAttempt = 0;
std::chrono::steady_clock::time_point nextAutomaticLaunch{};
uint64_t automaticObservedPid = 0;
std::chrono::steady_clock::time_point automaticObservedAt{};
std::chrono::steady_clock::time_point nextInputClaim{};
std::chrono::steady_clock::time_point nextServiceSupervision{};

void claimExclusiveInput(bool force = false) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && now < nextInputClaim) return;
  tsl::hlp::requestForeground(true, false);
  nextInputClaim = now + InputClaimInterval;
}

std::string readLastLine(const char* path, const char* unavailable) {
  FILE* file = std::fopen(path, "r");
  if (!file) return unavailable;

  char line[96] = {};
  char last[96] = {};
  while (std::fgets(line, sizeof(line), file)) {
    std::snprintf(last, sizeof(last), "%s", line);
  }
  std::fclose(file);

  std::string value = last[0] ? last : "Empty";
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r'))
    value.pop_back();
  return value;
}

std::string readTextFile(const char* path, size_t maximum = 1024) {
  FILE* file = std::fopen(path, "rb");
  if (!file) return {};
  std::string value;
  value.resize(maximum);
  const size_t length = std::fread(value.data(), 1, value.size(), file);
  std::fclose(file);
  value.resize(length);
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' ||
          value.back() == '\0'))
    value.pop_back();
  return value;
}

std::string resultText(Result result) {
  if (result == KERNELRESULT(OutOfResource))
    return "System resources exhausted";
  char text[40];
  std::snprintf(text, sizeof(text), "0x%08X (M%u D%u)", result,
                R_MODULE(result), R_DESCRIPTION(result));
  return text;
}

std::string formatTime(uint32_t milliseconds) {
  const uint32_t seconds = milliseconds / 1000;
  char text[24];
  std::snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
  return text;
}

std::string boundedText(const char* value, size_t capacity,
                        const char* fallback = "-") {
  const size_t length = strnlen(value, capacity);
  return length == 0 ? fallback : std::string(value, length);
}

bool processIsRunning(uint64_t* pidOut = nullptr) {
  uint64_t pid = 0;
  const Result result = pmdmntGetProcessId(&pid, SwitifyProgramId);
  if (R_FAILED(result) || pid == 0) return false;
  if (pidOut) *pidOut = pid;
  return true;
}

bool internalIpcIsReady(uint64_t pid) {
  unsigned long long markerPid = 0;
  unsigned result = UINT32_MAX;
  const int parsed = std::sscanf(readyText.c_str(), "pass-v043 %llX 0x%X",
                                 &markerPid, &result);
  return parsed == 2 && markerPid == pid && result == 0;
}

void disconnectIpc() {
  switifyIpcExit();
  connectedPid = 0;
  capabilities = 0;
  std::memset(&currentStatus, 0, sizeof(currentStatus));
}

std::string engineStateText(const SwitifyIpcStatus& status) {
  switch (status.service_state) {
    case SwitifyServiceState_Booting:
      return "Starting Spotify engine";
    case SwitifyServiceState_WaitingForCredentials:
      return "Sign in with the Switify app first";
    case SwitifyServiceState_Pairing:
      return "Waiting for Spotify Connect";
    case SwitifyServiceState_Connecting:
      return "Connecting to Spotify";
    case SwitifyServiceState_Ready:
      return (status.flags & SwitifyStatusFlag_Connected) ? "Online"
                                                         : "Ready";
    case SwitifyServiceState_Error:
      return "Spotify engine error";
    case SwitifyServiceState_Stopping:
      return "Stopping";
    default:
      return "Unknown state";
  }
}

std::string queueText(const SwitifyIpcStatus& status) {
  if (status.queue_count == 0 ||
      status.queue_index == SWITIFY_IPC_INVALID_INDEX)
    return "-";
  return std::to_string(status.queue_index + 1) + " of " +
         std::to_string(status.queue_count);
}

bool controlsAvailable() {
  return connectionState == ConnectionState::Connected &&
         currentStatus.service_state == SwitifyServiceState_Ready &&
         (capabilities & SwitifyIpcCapability_PlaybackControl) != 0;
}

bool gameVolumeAvailable() {
  return connectionState == ConnectionState::Connected &&
         (capabilities & SwitifyIpcCapability_GameVolume) != 0;
}

void refreshServiceState(bool updateFeedback = true) {
  uint64_t pid = 0;
  if (!processIsRunning(&pid)) {
    disconnectIpc();
    runningPid = 0;
    connectionState = ConnectionState::Dormant;
    serviceText = "Dormant";
    readyText = "Not started";
    if (updateFeedback) feedbackText = "Background service is not running";
    return;
  }

  if (connectedPid != 0 && connectedPid != pid) disconnectIpc();
  runningPid = pid;
  readyText = readLastLine(ReadyPath, "Waiting for readiness check");
  if (!internalIpcIsReady(pid)) {
    disconnectIpc();
    connectionState = ConnectionState::Starting;
    serviceText = "Starting";
    if (updateFeedback) feedbackText = readyText;
    return;
  }

  // Atmosphere can defer GetService while a service is absent. The readiness
  // marker contains this process's PID and is written only after an internal
  // Ping succeeds, so this call cannot freeze the Ultrahand host.
  if (!switifyIpcIsActive()) {
    const Result connectResult = switifyIpcInitialize();
    if (R_FAILED(connectResult)) {
      connectionState = ConnectionState::Failed;
      serviceText = "IPC connection failed";
      if (updateFeedback) feedbackText = resultText(connectResult);
      return;
    }
    connectedPid = pid;
  }

  uint32_t version = 0;
  SwitifyIpcStatus status{};
  Result result = switifyIpcPing();
  if (R_SUCCEEDED(result)) result = switifyIpcGetApiVersion(&version);
  if (R_SUCCEEDED(result)) result = switifyIpcGetCapabilities(&capabilities);
  if (R_SUCCEEDED(result)) result = switifyIpcGetStatus(&status);
  if (R_FAILED(result)) {
    disconnectIpc();
    connectionState = ConnectionState::Failed;
    serviceText = "IPC request failed";
    if (updateFeedback) feedbackText = resultText(result);
    return;
  }

  if (version != SWITIFY_IPC_API_VERSION ||
      status.struct_size != sizeof(status) ||
      status.abi_version != SWITIFY_IPC_STATUS_ABI_VERSION ||
      !(capabilities & SwitifyIpcCapability_Status)) {
    connectionState = ConnectionState::Incompatible;
    serviceText = "Update mismatch";
    if (updateFeedback) feedbackText = "Overlay and background versions differ";
    return;
  }

  currentStatus = status;
  connectionState = ConnectionState::Connected;
  serviceText = engineStateText(status);
  if (updateFeedback) feedbackText = "Background connection is healthy";
}

void launchService() {
  refreshServiceState(false);
  if (runningPid != 0) {
    feedbackText = "Background playback is already running";
    return;
  }

  std::remove(ReadyPath);
  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    serviceText = "Launch failed";
    feedbackText = "pm:shell " + resultText(result);
    return;
  }

  NcmProgramLocation location{};
  location.program_id = SwitifyProgramId;
  location.storageID = NcmStorageId_None;
  uint64_t pid = 0;
  result = pmshellLaunchProgram(0, &location, &pid);
  pmshellExit();
  if (R_FAILED(result) || pid == 0) {
    serviceText = "Launch failed";
    feedbackText = resultText(result);
    return;
  }

  runningPid = pid;
  connectionState = ConnectionState::Starting;
  serviceText = "Starting";
  feedbackText = "Starting Spotify in the background";
  readyText = "Waiting for readiness check";
}

void beginAutomaticStartup() {
  automaticLaunchActive = true;
  const auto now = std::chrono::steady_clock::now();
  // A process discovered without a working readiness marker is an existing
  // startup attempt, not proof that startup succeeded. The old state machine
  // returned forever merely because a PID existed.
  automaticLaunchAttempt = runningPid == 0 ? 0 : 1;
  automaticObservedPid = runningPid;
  automaticObservedAt = now;
  nextAutomaticLaunch = now;
}

bool terminateServiceForAutomaticRecovery() {
  disconnectIpc();
  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    feedbackText = "Automatic recovery: pm:shell " + resultText(result);
    return false;
  }
  result = pmshellTerminateProgram(SwitifyProgramId);
  pmshellExit();
  if (R_FAILED(result)) {
    feedbackText = "Automatic recovery stop failed: " + resultText(result);
    return false;
  }

  runningPid = 0;
  automaticObservedPid = 0;
  connectionState = ConnectionState::Dormant;
  serviceText = "Restarting";
  readyText = "Restarting after readiness timeout";
  return true;
}

void maintainAutomaticStartup() {
  if (!automaticLaunchActive) return;
  const auto now = std::chrono::steady_clock::now();
  if (connectionState == ConnectionState::Connected) {
    // Connected is the healthy state of a persistent supervisor, not the end
    // of automatic startup. Reset its retry budget so an unexpected service
    // exit during Home/Library/Search can be relaunched immediately.
    automaticLaunchAttempt = 0;
    automaticObservedPid = runningPid;
    automaticObservedAt = now;
    nextAutomaticLaunch = now;
    return;
  }
  if (runningPid != 0) {
    if (automaticObservedPid != runningPid) {
      automaticObservedPid = runningPid;
      automaticObservedAt = now;
      if (automaticLaunchAttempt == 0) automaticLaunchAttempt = 1;
      return;
    }
    if (automaticObservedAt.time_since_epoch().count() == 0)
      automaticObservedAt = now;
    if (now - automaticObservedAt < StartupReadinessTimeout) return;

    const bool exhausted = automaticLaunchAttempt >= AutomaticLaunchAttempts;
    if (!terminateServiceForAutomaticRecovery()) {
      automaticLaunchActive = false;
      serviceText = "Startup recovery failed";
      return;
    }
    if (exhausted) {
      automaticLaunchActive = false;
      serviceText = "Startup failed";
      feedbackText = "Readiness timed out after automatic retries";
      return;
    }
    feedbackText = "Startup timed out; relaunching automatically";
    nextAutomaticLaunch = now + StartupRelaunchDelay;
    return;
  }

  if (now < nextAutomaticLaunch) return;
  if (automaticLaunchAttempt >= AutomaticLaunchAttempts) {
    automaticLaunchActive = false;
    feedbackText = "Automatic startup failed; open Settings to retry";
    return;
  }

  ++automaticLaunchAttempt;
  launchService();
  if (runningPid != 0) {
    automaticObservedPid = runningPid;
    automaticObservedAt = now;
    return;
  }

  const std::string failure = feedbackText;
  if (automaticLaunchAttempt >= AutomaticLaunchAttempts) {
    automaticLaunchActive = false;
    feedbackText = failure + "; automatic retries exhausted";
    return;
  }
  const unsigned delaySeconds = 1U << (automaticLaunchAttempt - 1);
  nextAutomaticLaunch = now + std::chrono::seconds(delaySeconds);
  feedbackText = failure + "; retrying automatically";
}

void superviseAutomaticService(bool force = false) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && now < nextServiceSupervision) return;
  refreshServiceState(false);
  maintainAutomaticStartup();
  nextServiceSupervision = now + RefreshInterval;
}

void stopService() {
  automaticLaunchActive = false;
  automaticObservedPid = 0;
  if (!processIsRunning()) {
    disconnectIpc();
    runningPid = 0;
    connectionState = ConnectionState::Dormant;
    serviceText = "Dormant";
    feedbackText = "Background playback is already stopped";
    return;
  }

  // Ask the worker to remove attenuation before pm:shell forcibly terminates
  // it. A forced process stop does not run C++ destructors.
  if (switifyIpcIsActive() &&
      (capabilities & SwitifyIpcCapability_GameVolume) != 0 &&
      R_SUCCEEDED(switifyIpcRestoreGameVolume()))
    svcSleepThread(UINT64_C(150000000));
  disconnectIpc();

  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    serviceText = "Stop failed";
    feedbackText = "pm:shell " + resultText(result);
    return;
  }
  result = pmshellTerminateProgram(SwitifyProgramId);
  pmshellExit();
  if (R_FAILED(result)) {
    serviceText = "Stop failed";
    feedbackText = resultText(result);
    return;
  }

  runningPid = 0;
  connectionState = ConnectionState::Dormant;
  serviceText = "Dormant";
  feedbackText = "Background playback stopped";
}

template <typename Command>
bool dispatchControl(const char* description, Command command) {
  if (!controlsAvailable()) {
    feedbackText = "Playback controls are not ready";
    return false;
  }
  const Result result = command();
  feedbackText = R_SUCCEEDED(result) ? description : resultText(result);
  return R_SUCCEEDED(result);
}

template <typename Command>
bool dispatchGameVolume(const char* description, Command command) {
  if (!gameVolumeAvailable()) {
    feedbackText = "Game volume is unavailable (aud:a may be in use)";
    return false;
  }
  const Result result = command();
  feedbackText = R_SUCCEEDED(result) ? description : resultText(result);
  return R_SUCCEEDED(result);
}

std::string clipped(std::string value, size_t maximum) {
  if (value.size() <= maximum) return value;
  if (maximum < 4) return value.substr(0, maximum);
  value.resize(maximum - 3);
  return value + "...";
}

std::string browseText(const char* value, size_t capacity) {
  return boundedText(value, capacity, "");
}

template <typename Action>
tsl::elm::ListItem* actionItem(const std::string& text,
                               const std::string& value, Action action) {
  auto* item = new tsl::elm::ListItem(text, value);
  item->setClickListener([action = std::move(action)](uint64_t keys) {
    if (!(keys & HidNpadButton_A)) return false;
    action();
    return true;
  });
  return item;
}

void drawOutline(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                 s32 height, tsl::Color color, s32 thickness = 2) {
  renderer->drawRect(x, y, width, thickness, color);
  renderer->drawRect(x, y + height - thickness, width, thickness, color);
  renderer->drawRect(x, y, thickness, height, color);
  renderer->drawRect(x + width - thickness, y, thickness, height, color);
}

class PlayerElement final : public tsl::elm::Element {
 public:
  static constexpr s32 Height = 520;

  tsl::elm::Element* requestFocus(tsl::elm::Element*,
                                  tsl::FocusDirection) override {
    return this;
  }

  void layout(u16, u16, u16, u16) override {
    setBoundaries(getX(), getY(), getWidth(), Height);
  }

  void sync() {
    const std::string next = readTextFile(ArtworkUrlPath);
    if (next != artworkUrl) artworkUrl = next;

    // Starting artwork networking here rather than in initServices() leaves
    // the launch path free of the artwork BSD session and worker thread.  A
    // repeated request is cheap and lets artwork start as soon as a previously
    // booting service becomes healthy.
    if (connectionState == ConnectionState::Connected &&
        currentStatus.service_state == SwitifyServiceState_Ready &&
        !artworkUrl.empty()) {
      switify::artwork::request(artworkUrl, ArtSize);
    }
  }

  bool onClick(u64 keys) override {
    if (keys & HidNpadButton_Up) {
      if (focus < 5) {
        focus = 5;
        return true;
      }
      if (focus == 6) {
        focus = 2;
        return true;
      }
      if (focus == 7) {
        focus = 6;
        return true;
      }
      return false;
    }
    if (keys & HidNpadButton_Down) {
      if (focus == 5) {
        focus = 2;
        return true;
      }
      if (focus < 5) {
        focus = 6;
        return true;
      }
      if (focus == 6 && gameVolumeAvailable()) {
        focus = 7;
        return true;
      }
      return false;
    }
    if (focus == 5) {
      if (keys & HidNpadButton_Left) {
        seekBy(-15000);
        return true;
      }
      if (keys & HidNpadButton_Right) {
        seekBy(15000);
        return true;
      }
      return false;
    }
    if (focus == 6) {
      if (keys & HidNpadButton_Left) {
        changeVolume(-50);
        return true;
      }
      if (keys & HidNpadButton_Right) {
        changeVolume(50);
        return true;
      }
      return false;
    }
    if (focus == 7) {
      if (keys & HidNpadButton_Left) {
        changeGameVolume(-50);
        return true;
      }
      if (keys & HidNpadButton_Right) {
        changeGameVolume(50);
        return true;
      }
      return false;
    }
    if (keys & HidNpadButton_Left) {
      if (focus > 0) --focus;
      return true;
    }
    if (keys & HidNpadButton_Right) {
      if (focus < 4) ++focus;
      return true;
    }
    if (keys & HidNpadButton_A) {
      activate(focus);
      return true;
    }
    if (keys & HidNpadButton_X) {
      toggleRepeat();
      return true;
    }
    if (keys & HidNpadButton_Y) {
      toggleShuffle();
      return true;
    }
    return false;
  }

  void draw(tsl::gfx::Renderer* renderer) override {
    const tsl::Color green = a({0x1, 0xD, 0x6, 0xF});
    const tsl::Color dimGreen = a({0x1, 0x8, 0x5, 0xA});
    const tsl::Color text = a(tsl::style::color::ColorText);
    const tsl::Color dim = a(tsl::style::color::ColorHeaderBar);
    const tsl::Color panel = a(tsl::style::color::ColorFrame);
    const s32 x = getX();
    const s32 y = getY();
    const s32 width = getWidth();
    const s32 center = x + width / 2;
    const s32 artX = center - ArtSize / 2;
    const s32 artY = y + 14;

    renderer->drawRect(x + 4, y + 4, width - 8, Height - 8, panel);
    drawOutline(renderer, x + 4, y + 4, width - 8, Height - 8,
                hasFocus() ? green : dimGreen, hasFocus() ? 3 : 2);

    const uint8_t* pixels = nullptr;
    int artWidth = 0;
    int artHeight = 0;
    switify::artwork::borrow(&pixels, &artWidth, &artHeight);
    if (pixels && artWidth > 0 && artHeight > 0) {
      const s32 decodedX = artX + (ArtSize - artWidth) / 2;
      const s32 decodedY = artY + (ArtSize - artHeight) / 2;
      renderer->drawBitmap(decodedX, decodedY, artWidth, artHeight, pixels);
    } else {
      renderer->drawRect(artX, artY, ArtSize, ArtSize,
                         a({0x1, 0x2, 0x2, 0xF}));
      drawOutline(renderer, artX, artY, ArtSize, ArtSize, dimGreen);
      renderer->drawLine(center + 18, artY + 70, center + 18,
                         artY + 142, green);
      renderer->drawLine(center + 18, artY + 70, center + 58,
                         artY + 61, green);
      renderer->drawCircle(center + 3, artY + 148, 15, true, green);
    }

    const bool hasTrack = currentStatus.flags & SwitifyStatusFlag_HasTrack;
    const std::string title = clipped(
        hasTrack ? boundedText(currentStatus.title, sizeof(currentStatus.title))
                 : "Choose something to play",
        31);
    const std::string artist = clipped(
        hasTrack ? boundedText(currentStatus.artist,
                               sizeof(currentStatus.artist))
                 : serviceText,
        36);
    centeredText(renderer, title, center, artY + ArtSize + 31, 23, text);
    centeredText(renderer, artist, center, artY + ArtSize + 54, 17, dim);

    const s32 barX = x + 20;
    const s32 barY = artY + ArtSize + 78;
    const s32 barWidth = width - 40;
    const float fraction = currentStatus.duration_ms == 0
                               ? 0.0f
                               : std::clamp(
                                     static_cast<float>(currentStatus.position_ms) /
                                         currentStatus.duration_ms,
                                     0.0f, 1.0f);
    if (hasFocus() && focus == 5)
      renderer->drawRect(barX - 2, barY - 4, barWidth + 4, 11,
                         a({0x1, 0xD, 0x6, 0x7}));
    renderer->drawRect(barX, barY, barWidth, 3, a({0xA, 0xA, 0xA, 0xA}));
    const s32 fill = static_cast<s32>(barWidth * fraction);
    if (fill > 0) renderer->drawRect(barX, barY - 2, fill, 7, green);
    renderer->drawCircle(barX + fill, barY + 1, 6, true, green);
    renderer->drawString(formatTime(currentStatus.position_ms), false, barX,
                         barY + 26, 16, text);
    const std::string total = formatTime(currentStatus.duration_ms);
    const auto [totalWidth, totalHeight] = renderer->drawString(
        total, false, 0, 0, 16, tsl::style::color::ColorTransparent);
    (void)totalHeight;
    renderer->drawString(total, false, barX + barWidth - totalWidth,
                         barY + 26, 16, text);

    const s32 controlsY = artY + ArtSize + 139;
    const s32 spacing = (width - 70) / 4;
    for (int index = 0; index < 5; ++index) {
      const s32 controlX = x + 35 + spacing * index;
      if (hasFocus() && focus == index)
        renderer->drawRect(controlX - 20, controlsY - 20, 40, 40,
                           a({0x1, 0xD, 0x6, 0x5}));
      drawControl(renderer, index, controlX, controlsY,
                  index == 0 &&
                          (currentStatus.flags &
                           (SwitifyStatusFlag_RepeatAll |
                            SwitifyStatusFlag_RepeatOne))
                      ? green
                      : (index == 4 &&
                                 (currentStatus.flags &
                                  SwitifyStatusFlag_Shuffle)
                             ? green
                             : text));
    }

    const s32 volumePercentY = controlsY + 43;
    const s32 volumeBarX = x + 112;
    const s32 volumeBarY = controlsY + 66;
    const s32 volumeBarWidth = width - 138;
    const uint32_t volume = std::min(UINT32_C(1000),
                                     currentStatus.volume_permille);
    // Keep the label on the slider's row. Only the percentage belongs above.
    renderer->drawString("Music", false, x + 20, volumeBarY + 6, 16, text);
    const std::string volumeText = std::to_string(volume / 10) + "%";
    const auto [volumeTextWidth, volumeTextHeight] = renderer->drawString(
        volumeText, false, 0, 0, 16, tsl::style::color::ColorTransparent);
    (void)volumeTextHeight;
    renderer->drawString(volumeText, false, x + width - 20 - volumeTextWidth,
                         volumePercentY, 16, text);
    if (hasFocus() && focus == 6)
      renderer->drawRect(volumeBarX - 3, volumeBarY - 5,
                         volumeBarWidth + 6, 13, a({0x1, 0xD, 0x6, 0x7}));
    renderer->drawRect(volumeBarX, volumeBarY, volumeBarWidth, 3,
                       a({0xA, 0xA, 0xA, 0xA}));
    const s32 volumeFill = static_cast<s32>(volumeBarWidth * volume / 1000.0f);
    if (volumeFill > 0)
      renderer->drawRect(volumeBarX, volumeBarY - 2, volumeFill, 7, green);
    renderer->drawCircle(volumeBarX + volumeFill, volumeBarY + 1, 6, true,
                         green);

    const bool gameControl = gameVolumeAvailable();
    const bool gameRunning =
        currentStatus.flags & SwitifyStatusFlag_GameRunning;
    const s32 gamePercentY = volumePercentY + 44;
    const s32 gameBarY = volumeBarY + 44;
    const uint32_t gameVolume = std::min(
        UINT32_C(1000), currentStatus.game_volume_permille);
    const tsl::Color gameColor = gameControl ? green : dimGreen;
    const tsl::Color gameTextColor = gameControl ? text : dim;
    renderer->drawString(gameRunning ? "Game" : "Game (next)", false,
                         x + 20, gameBarY + 6, 16, gameTextColor);
    const std::string gameVolumeText =
        gameControl ? std::to_string(gameVolume / 10) + "%" : "Unavailable";
    const auto [gameTextWidth, gameTextHeight] = renderer->drawString(
        gameVolumeText, false, 0, 0, 16,
        tsl::style::color::ColorTransparent);
    (void)gameTextHeight;
    renderer->drawString(gameVolumeText, false,
                         x + width - 20 - gameTextWidth, gamePercentY, 16,
                         gameTextColor);
    if (hasFocus() && focus == 7)
      renderer->drawRect(volumeBarX - 3, gameBarY - 5,
                         volumeBarWidth + 6, 13, a({0x1, 0xD, 0x6, 0x7}));
    renderer->drawRect(volumeBarX, gameBarY, volumeBarWidth, 3,
                       a({0xA, 0xA, 0xA, 0xA}));
    const s32 gameFill =
        static_cast<s32>(volumeBarWidth * gameVolume / 1000.0f);
    if (gameFill > 0)
      renderer->drawRect(volumeBarX, gameBarY - 2, gameFill, 7, gameColor);
    renderer->drawCircle(volumeBarX + gameFill, gameBarY + 1, 6, true,
                         gameColor);
  }

 private:
  static constexpr s32 ArtSize = 176;

  void centeredText(tsl::gfx::Renderer* renderer, const std::string& value,
                    s32 center, s32 baseline, u32 size,
                    tsl::Color color) const {
    const auto [width, height] = renderer->drawString(
        value, false, 0, 0, size, tsl::style::color::ColorTransparent);
    (void)height;
    renderer->drawString(value, false, center - width / 2, baseline, size,
                         color);
  }

  void drawControl(tsl::gfx::Renderer* renderer, int index, s32 x, s32 y,
                   tsl::Color color) const {
    if (index == 0) {
      renderer->drawLine(x - 13, y - 7, x + 11, y - 7, color);
      renderer->drawLine(x + 11, y - 7, x + 6, y - 12, color);
      renderer->drawLine(x + 11, y - 7, x + 6, y - 2, color);
      renderer->drawLine(x + 13, y + 7, x - 11, y + 7, color);
      renderer->drawLine(x - 11, y + 7, x - 6, y + 2, color);
      renderer->drawLine(x - 11, y + 7, x - 6, y + 12, color);
      if (currentStatus.flags & SwitifyStatusFlag_RepeatOne)
        renderer->drawString("1", false, x + 8, y + 2, 12, color);
    } else if (index == 1 || index == 3) {
      const bool next = index == 3;
      renderer->drawRect(x + (next ? 10 : -12), y - 12, 3, 24, color);
      for (s32 line = 0; line <= 20; ++line) {
        const s32 half = next ? (20 - line) * 12 / 20
                              : line * 12 / 20;
        const s32 px = x - 10 + line;
        renderer->drawLine(px, y - half, px, y + half, color);
      }
    } else if (index == 2) {
      if (currentStatus.playback_state == SwitifyPlaybackState_Playing) {
        renderer->drawRect(x - 9, y - 13, 6, 26, color);
        renderer->drawRect(x + 4, y - 13, 6, 26, color);
      } else {
        for (s32 line = 0; line <= 24; ++line) {
          const s32 half = (24 - line) * 13 / 24;
          renderer->drawLine(x - 10 + line, y - half, x - 10 + line,
                             y + half, color);
        }
      }
    } else {
      renderer->drawLine(x - 14, y - 9, x - 5, y - 9, color);
      renderer->drawLine(x - 5, y - 9, x + 7, y + 9, color);
      renderer->drawLine(x + 7, y + 9, x + 14, y + 9, color);
      renderer->drawLine(x + 14, y + 9, x + 9, y + 4, color);
      renderer->drawLine(x + 14, y + 9, x + 9, y + 14, color);
      renderer->drawLine(x - 14, y + 9, x - 5, y + 9, color);
      renderer->drawLine(x - 5, y + 9, x + 7, y - 9, color);
      renderer->drawLine(x + 7, y - 9, x + 14, y - 9, color);
      renderer->drawLine(x + 14, y - 9, x + 9, y - 14, color);
      renderer->drawLine(x + 14, y - 9, x + 9, y - 4, color);
    }
  }

  void seekBy(int32_t delta) {
    if (!(currentStatus.flags & SwitifyStatusFlag_CanSeek)) return;
    const int64_t target = std::clamp<int64_t>(
        static_cast<int64_t>(currentStatus.position_ms) + delta, 0,
        currentStatus.duration_ms);
    if (dispatchControl("Position changed", [target] {
          return switifyIpcSeek(static_cast<uint32_t>(target));
        }))
      currentStatus.position_ms = static_cast<uint32_t>(target);
  }

  void changeVolume(int32_t delta) {
    const int32_t value = std::clamp<int32_t>(
        static_cast<int32_t>(currentStatus.volume_permille) + delta, 0, 1000);
    if (dispatchControl("Volume changed", [value] {
          return switifyIpcSetVolume(static_cast<uint32_t>(value));
        }))
      currentStatus.volume_permille = static_cast<uint32_t>(value);
  }

  void changeGameVolume(int32_t delta) {
    const int32_t value = std::clamp<int32_t>(
        static_cast<int32_t>(currentStatus.game_volume_permille) + delta, 0,
        1000);
    if (dispatchGameVolume("Game volume changed", [value] {
          return switifyIpcSetGameVolume(static_cast<uint32_t>(value));
        }))
      currentStatus.game_volume_permille = static_cast<uint32_t>(value);
  }

  void toggleRepeat() {
    SwitifyRepeatMode mode = SwitifyRepeatMode_All;
    const char* message = "Repeat all";
    if (currentStatus.flags & SwitifyStatusFlag_RepeatAll) {
      mode = SwitifyRepeatMode_One;
      message = "Repeat one";
    } else if (currentStatus.flags & SwitifyStatusFlag_RepeatOne) {
      mode = SwitifyRepeatMode_Off;
      message = "Repeat off";
    }
    if (dispatchControl(message,
                        [mode] { return switifyIpcSetRepeatMode(mode); })) {
      currentStatus.flags &= ~(SwitifyStatusFlag_RepeatAll |
                               SwitifyStatusFlag_RepeatOne);
      if (mode == SwitifyRepeatMode_All)
        currentStatus.flags |= SwitifyStatusFlag_RepeatAll;
      else if (mode == SwitifyRepeatMode_One)
        currentStatus.flags |= SwitifyStatusFlag_RepeatOne;
    }
  }

  void toggleShuffle() {
    const bool enabled = !(currentStatus.flags & SwitifyStatusFlag_Shuffle);
    if (dispatchControl(enabled ? "Shuffle enabled" : "Shuffle disabled",
                        [enabled] { return switifyIpcSetShuffle(enabled); })) {
      if (enabled)
        currentStatus.flags |= SwitifyStatusFlag_Shuffle;
      else
        currentStatus.flags &= ~SwitifyStatusFlag_Shuffle;
    }
  }

  void activate(int index) {
    switch (index) {
      case 0:
        toggleRepeat();
        break;
      case 1:
        dispatchControl("Previous track", switifyIpcPrevious);
        break;
      case 2: {
        const bool playing = currentStatus.playback_state ==
                             SwitifyPlaybackState_Playing;
        if (dispatchControl(playing ? "Paused" : "Playing",
                            playing ? switifyIpcPause : switifyIpcPlay)) {
          currentStatus.playback_state =
              playing ? SwitifyPlaybackState_Paused
                      : SwitifyPlaybackState_Playing;
        }
        break;
      }
      case 3:
        dispatchControl("Next track", switifyIpcNext);
        break;
      case 4:
        toggleShuffle();
        break;
    }
  }

  int focus = 2;
  std::string artworkUrl;
};

uint64_t currentBrowseGeneration() {
  SwitifyIpcBrowseHeader header{};
  return R_SUCCEEDED(switifyIpcGetBrowseHeader(&header)) ? header.generation
                                                         : 0;
}

class BrowseGui final : public tsl::Gui {
 public:
  explicit BrowseGui(uint64_t previousGeneration = 0)
      : previousGeneration(previousGeneration) {}

  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("Switify", "Spotify browser");
    auto* list = new tsl::elm::List();
    Result result = switifyIpcGetBrowseHeader(&header);
    if (R_FAILED(result) ||
        header.struct_size != sizeof(SwitifyIpcBrowseHeader) ||
        header.abi_version != SWITIFY_IPC_BROWSE_ABI_VERSION) {
      renderedUnavailable = true;
      list->addItem(new tsl::elm::CategoryHeader("Browser unavailable"));
      list->addItem(new tsl::elm::ListItem(
          automaticLaunchActive ? "Service is restarting automatically"
                                : "Could not read Spotify content",
          resultText(result)));
    } else if (header.generation == previousGeneration &&
               header.state != SwitifyBrowseState_Loading) {
      list->addItem(new tsl::elm::CategoryHeader("Opening Spotify"));
      list->addItem(new tsl::elm::ListItem("Loading...", "Please wait"));
    } else {
      const std::string title = browseText(header.title, sizeof(header.title));
      const std::string subtitle =
          browseText(header.subtitle, sizeof(header.subtitle));
      list->addItem(new tsl::elm::CategoryHeader(
          title.empty() ? "Spotify" : title));
      if (!subtitle.empty())
        list->addItem(
            new tsl::elm::CategoryHeader(clipped(subtitle, 42)));
      if (header.state == SwitifyBrowseState_Loading) {
        list->addItem(new tsl::elm::ListItem("Loading Spotify content...", ""));
      } else if (header.state == SwitifyBrowseState_Error) {
        list->addItem(new tsl::elm::ListItem(
            clipped(browseText(header.message, sizeof(header.message)), 44),
            "B Back"));
      } else if (header.state == SwitifyBrowseState_Ready) {
        populateItems(list);
      } else {
        list->addItem(new tsl::elm::ListItem("Nothing to show yet", ""));
      }
    }
    renderedGeneration = header.generation;
    renderedState = header.state;
    renderedCount = header.item_count;
    frame->setContent(list);
    nextRefresh = std::chrono::steady_clock::now() + BrowseRefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    if (connectionState != ConnectionState::Connected) {
      if (!renderedUnavailable) {
        tsl::swapTo<BrowseGui>(previousGeneration);
        return;
      }
      nextRefresh = now + BrowseRefreshInterval;
      return;
    }
    if (renderedUnavailable) {
      tsl::swapTo<BrowseGui>(previousGeneration);
      return;
    }
    SwitifyIpcBrowseHeader latest{};
    if (R_SUCCEEDED(switifyIpcGetBrowseHeader(&latest)) &&
        (latest.generation != renderedGeneration ||
         latest.state != renderedState || latest.item_count != renderedCount)) {
      tsl::swapTo<BrowseGui>(previousGeneration);
      return;
    }
    nextRefresh = now + BrowseRefreshInterval;
  }

  bool handleInput(u64 keysDown, u64, const HidTouchState&,
                   HidAnalogStickState, HidAnalogStickState) override {
    if (keysDown & HidNpadButton_B) {
      // A network request must never trap the user inside Switify. Returning
      // false lets libtesla pop this GUI immediately without waiting for IPC.
      // Re-entering Home/Library/Search starts a fresh root request.
      if (renderedState == SwitifyBrowseState_Loading) return false;

      // The service can advance the browse generation while a large collection
      // is loading or being paged.  Never send Back with the GUI's cached
      // generation: that turns a perfectly valid B press into a silent no-op.
      SwitifyIpcBrowseHeader latest{};
      const Result headerResult = switifyIpcGetBrowseHeader(&latest);
      if (R_FAILED(headerResult) ||
          latest.struct_size != sizeof(SwitifyIpcBrowseHeader) ||
          latest.abi_version != SWITIFY_IPC_BROWSE_ABI_VERSION) {
        feedbackText = resultText(headerResult);
        return false;
      }
      if (latest.depth == 0) return false;

      const uint64_t old = latest.generation;
      const Result result = switifyIpcBrowseBack(old);
      if (R_SUCCEEDED(result)) {
        previousGeneration = old;
        nextRefresh = std::chrono::steady_clock::now();
      } else {
        feedbackText = resultText(result);
        return false;
      }
      return true;
    }
    return false;
  }

 private:
  void populateItems(tsl::elm::List* list) {
    if (header.item_count == 0) {
      list->addItem(new tsl::elm::ListItem("No results", "B Back"));
      return;
    }
    const uint32_t count = std::min<uint32_t>(header.item_count, 128);
    for (uint32_t index = 0; index < count; ++index) {
      SwitifyIpcBrowseItem item{};
      if (R_FAILED(switifyIpcGetBrowseItem(index, &item)) ||
          item.generation != header.generation)
        continue;
      std::string name = browseText(item.name, sizeof(item.name));
      std::string value = browseText(item.subtitle, sizeof(item.subtitle));
      if (item.kind == SwitifyBrowseItemKind_Track) {
        if (!value.empty()) name += " - " + value;
        name = clipped(name, 42);
        value = item.duration_ms > 0 ? formatTime(item.duration_ms) : "";
      } else {
        name = clipped(name, 36);
        value = clipped(value, 18);
      }
      auto* row = actionItem(name.empty() ? "Untitled" : name, value,
                             [this, index, kind = item.kind] {
        const uint64_t old = header.generation;
        const Result result =
            kind == SwitifyBrowseItemKind_Track
                ? switifyIpcBrowsePlay(old, index)
                : switifyIpcBrowseOpen(old, index);
        if (R_FAILED(result)) {
          feedbackText = resultText(result);
        } else if (kind != SwitifyBrowseItemKind_Track) {
          previousGeneration = old;
          nextRefresh = std::chrono::steady_clock::now();
        }
      });
      list->addItem(row);
    }
  }

  uint64_t previousGeneration = 0;
  SwitifyIpcBrowseHeader header{};
  uint64_t renderedGeneration = 0;
  uint32_t renderedState = SwitifyBrowseState_Idle;
  uint32_t renderedCount = 0;
  bool renderedUnavailable = false;
  std::chrono::steady_clock::time_point nextRefresh{};
};

class SearchKeyboardElement final : public tsl::elm::Element {
 public:
  tsl::elm::Element* requestFocus(tsl::elm::Element*,
                                  tsl::FocusDirection) override {
    return this;
  }

  void layout(u16, u16, u16, u16) override {
    setBoundaries(getX(), getY(), getWidth(), getHeight());
  }

  bool onClick(u64 keys) override {
    if (keys & HidNpadButton_AnyUp) {
      if (row > 0) --row;
      clampColumn();
      return true;
    }
    if (keys & HidNpadButton_AnyDown) {
      if (row + 1 < static_cast<int>(Rows.size())) ++row;
      clampColumn();
      return true;
    }
    if (keys & HidNpadButton_AnyLeft) {
      if (column > 0) --column;
      return true;
    }
    if (keys & HidNpadButton_AnyRight) {
      if (column + 1 < static_cast<int>(Rows[row].size())) ++column;
      return true;
    }
    if (keys & HidNpadButton_X) {
      if (!query.empty()) query.pop_back();
      return true;
    }
    if (keys & HidNpadButton_A) {
      activate(Rows[row][column]);
      return true;
    }
    return false;
  }

  void draw(tsl::gfx::Renderer* renderer) override {
    const s32 x = getX();
    const s32 y = getY();
    const s32 width = getWidth();
    const tsl::Color green = a({0x1, 0xD, 0x6, 0xF});
    renderer->drawRect(x + 10, y + 14, width - 20, 43,
                       a(tsl::style::color::ColorFrame));
    renderer->drawString(query.empty() ? "Search Spotify" : clipped(query, 34),
                         false, x + 20, y + 43, 21,
                         a(tsl::style::color::ColorText));
    const s32 top = y + 76;
    const s32 rowHeight = 45;
    for (size_t r = 0; r < Rows.size(); ++r) {
      const auto& keys = Rows[r];
      const s32 cellWidth = (width - 20) / static_cast<s32>(keys.size());
      for (size_t c = 0; c < keys.size(); ++c) {
        const s32 keyX = x + 10 + static_cast<s32>(c) * cellWidth;
        const s32 keyY = top + static_cast<s32>(r) * rowHeight;
        renderer->drawRect(
            keyX + 2, keyY + 2, cellWidth - 4, rowHeight - 5,
            r == static_cast<size_t>(row) && c == static_cast<size_t>(column)
                ? green
                : a(tsl::style::color::ColorFrame));
        const std::string label = keys[c] == "SHIFT" && shifted
                                      ? "shift"
                                      : keys[c];
        const auto [textWidth, textHeight] = renderer->drawString(
            label, false, 0, 0, 16,
            tsl::style::color::ColorTransparent);
        (void)textHeight;
        renderer->drawString(label, false,
                             keyX + (cellWidth - textWidth) / 2, keyY + 27,
                             16, a(tsl::style::color::ColorText));
      }
    }
  }

 private:
  inline static const std::vector<std::vector<std::string>> Rows = {
      {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
      {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l"},
      {"z", "x", "c", "v", "b", "n", "m", "-"},
      {"SHIFT", "SPACE", "DEL", "SEARCH"}};

  void clampColumn() {
    column = std::clamp(column, 0, static_cast<int>(Rows[row].size()) - 1);
  }

  void activate(const std::string& key) {
    if (key == "SHIFT") {
      shifted = !shifted;
    } else if (key == "SPACE") {
      if (query.size() < SWITIFY_IPC_BROWSE_QUERY_SIZE - 1) query += ' ';
    } else if (key == "DEL") {
      if (!query.empty()) query.pop_back();
    } else if (key == "SEARCH") {
      if (query.empty()) return;
      const uint64_t old = currentBrowseGeneration();
      const Result result = switifyIpcBrowseSearch(query.c_str());
      if (R_SUCCEEDED(result))
        tsl::swapTo<BrowseGui>(old);
      else
        feedbackText = resultText(result);
    } else if (query.size() < SWITIFY_IPC_BROWSE_QUERY_SIZE - 1) {
      char character = key[0];
      if (shifted)
        character = static_cast<char>(
            std::toupper(static_cast<unsigned char>(character)));
      query += character;
      shifted = false;
    }
  }

  std::string query;
  int row = 1;
  int column = 0;
  bool shifted = false;
};

class SearchGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    // Foreground applications can reclaim controller input after Tesla's
    // initial hand-off. Search must own HID for its entire lifetime so typing
    // never repeats A/D-pad input in the application underneath.
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame(
        "Search", "A type   X backspace   B cancel");
    frame->setContent(new SearchKeyboardElement());
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
  }
};

void openBrowser(Result (*request)()) {
  superviseAutomaticService(true);
  if (connectionState != ConnectionState::Connected ||
      currentStatus.service_state != SwitifyServiceState_Ready ||
      !(capabilities & SwitifyIpcCapability_Browsing)) {
    feedbackText = "Spotify is still starting";
    return;
  }
  const uint64_t old = currentBrowseGeneration();
  const Result result = request();
  if (R_SUCCEEDED(result))
    tsl::changeTo<BrowseGui>(old);
  else
    feedbackText = resultText(result);
}

class ServiceGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("Switify", "Service & settings");
    auto* list = new tsl::elm::List();
    list->addItem(new tsl::elm::CategoryHeader("Background playback"));
    serviceItem = new tsl::elm::ListItem("Service", serviceText);
    feedbackItem = new tsl::elm::ListItem("Details", clipped(feedbackText, 26));
    list->addItem(serviceItem);
    list->addItem(feedbackItem);
    retryItem = actionItem("Retry automatic startup", "A", [this] {
      beginAutomaticStartup();
      maintainAutomaticStartup();
      sync();
    });
    stopItem = actionItem("Stop background playback", "A", [this] {
      stopService();
      sync();
    });
    list->addItem(retryItem);
    list->addItem(stopItem);
    list->addItem(actionItem("Refresh connection", "A", [this] {
      refreshServiceState();
      sync();
    }));
    list->addItem(new tsl::elm::CategoryHeader("Audio"));
    qualityItem = actionItem("Streaming quality", qualityText(), [this] {
      if (connectionState != ConnectionState::Connected ||
          !(capabilities & SwitifyIpcCapability_AudioSettings)) {
        feedbackText = "Update the overlay and background service together";
        sync();
        return;
      }
      const bool high =
          !(currentStatus.flags & SwitifyStatusFlag_HighQuality);
      const Result result = switifyIpcSetAudioQuality(high);
      if (R_SUCCEEDED(result)) {
        if (high)
          currentStatus.flags |= SwitifyStatusFlag_HighQuality;
        else
          currentStatus.flags &= ~SwitifyStatusFlag_HighQuality;
        feedbackText = high ? "High quality saved; applies next service start"
                            : "Normal quality saved; applies next service start";
      } else {
        feedbackText = resultText(result);
      }
      sync();
    });
    list->addItem(qualityItem);
    list->addItem(new tsl::elm::ListItem(
        "Normal 160 kbps / High 320 kbps", "Restart to apply"));
    list->addItem(new tsl::elm::CategoryHeader(
        "Sign in once with the standalone Switify app"));
    frame->setContent(list);
    sync();
    nextRefresh = std::chrono::steady_clock::now() + RefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    sync();
    nextRefresh = now + RefreshInterval;
  }

 private:
  void sync() {
    if (!serviceItem) return;
    serviceItem->setValue(serviceText);
    feedbackItem->setValue(clipped(feedbackText, 26));
    retryItem->isLocked = runningPid != 0;
    stopItem->isLocked = runningPid == 0;
    if (qualityItem) qualityItem->setValue(qualityText());
  }

  std::string qualityText() const {
    return (currentStatus.flags & SwitifyStatusFlag_HighQuality)
               ? "High · 320 kbps"
               : "Normal · 160 kbps";
  }

  std::chrono::steady_clock::time_point nextRefresh{};
  tsl::elm::ListItem* serviceItem = nullptr;
  tsl::elm::ListItem* feedbackItem = nullptr;
  tsl::elm::ListItem* retryItem = nullptr;
  tsl::elm::ListItem* stopItem = nullptr;
  tsl::elm::ListItem* qualityItem = nullptr;
};

class NowPlayingGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("Switify", "Now Playing");
    auto* list = new tsl::elm::List();
    player = new PlayerElement();
    list->addItem(player, PlayerElement::Height);
    list->addItem(new tsl::elm::CategoryHeader("Playback session"));
    list->addItem(actionItem("Playlist", queueText(currentStatus), [] {
      openBrowser(switifyIpcBrowseCurrentPlaylist);
    }));
    list->addItem(actionItem("Queue", "Coming up", [] {
      openBrowser(switifyIpcBrowseQueue);
    }));
    frame->setContent(list);
    player->sync();
    nextRefresh = std::chrono::steady_clock::now() + RefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    if (player) player->sync();
    nextRefresh = now + RefreshInterval;
  }

 private:
  PlayerElement* player = nullptr;
  std::chrono::steady_clock::time_point nextRefresh{};
};

class MainGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("Switify", OverlayVersion);
    auto* list = new tsl::elm::List();
    player = new PlayerElement();
    list->addItem(player, PlayerElement::Height);
    list->addItem(new tsl::elm::CategoryHeader("Browse Spotify"));
    list->addItem(actionItem("Home", "For you", [] {
      openBrowser(switifyIpcBrowseHome);
    }));
    list->addItem(actionItem("Search", "Tracks & more", [] {
      if (connectionState == ConnectionState::Connected &&
          currentStatus.service_state == SwitifyServiceState_Ready &&
          (capabilities & SwitifyIpcCapability_Search)) {
        // Consume the A press that opens Search before changing screens.
        claimExclusiveInput(true);
        tsl::changeTo<SearchGui>();
      } else {
        feedbackText = "Spotify is still starting";
      }
    }));
    list->addItem(actionItem("Your Library", "Saved music", [] {
      openBrowser(switifyIpcBrowseLibrary);
    }));
    list->addItem(actionItem("Now Playing", "Playlist & queue", [] {
      tsl::changeTo<NowPlayingGui>();
    }));
    settingsItem = actionItem("Settings", serviceText, [] {
      tsl::changeTo<ServiceGui>();
    });
    list->addItem(settingsItem);
    frame->setContent(list);
    player->sync();
    nextRefresh = std::chrono::steady_clock::now() + RefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    if (player) player->sync();
    if (settingsItem) settingsItem->setValue(serviceText);
    nextRefresh = now + RefreshInterval;
  }

 private:
  PlayerElement* player = nullptr;
  tsl::elm::ListItem* settingsItem = nullptr;
  std::chrono::steady_clock::time_point nextRefresh{};
};

class SwitifyOverlay final : public tsl::Overlay {
 public:
  void initServices() override {
    refreshServiceState(false);
    if (connectionState != ConnectionState::Connected &&
        connectionState != ConnectionState::Incompatible) {
      beginAutomaticStartup();
      maintainAutomaticStartup();
    }
  }

  void exitServices() override {
    switify::artwork::shutdown();
    disconnectIpc();
  }

  void onHide() override {
    // The playback engine is a separate Atmosphere process and continues
    // normally. Release every overlay-owned worker, BSD session, and IPC
    // handle before Ultrahand returns to the game or HOME menu.
    switify::artwork::shutdown();
    disconnectIpc();
  }

  void onShow() override {
    refreshServiceState(false);
    if (connectionState != ConnectionState::Connected &&
        connectionState != ConnectionState::Incompatible) {
      beginAutomaticStartup();
      maintainAutomaticStartup();
    }
  }

  std::unique_ptr<tsl::Gui> loadInitialGui() override {
    return initially<MainGui>();
  }
};

}  // namespace

int main(int argc, char** argv) {
  return tsl::loop<SwitifyOverlay>(argc, argv);
}
