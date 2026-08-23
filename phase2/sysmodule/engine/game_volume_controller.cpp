#include "game_volume_controller.hpp"

#include "NXConsoleLog.h"

#include <algorithm>
#include <switch.h>

namespace switify::phase2 {
namespace {

constexpr auto ProcessPollInterval = std::chrono::milliseconds(250);
constexpr uint64_t FadeDurationNs = UINT64_C(50000000);

void initializeHosVersion() {
  if (hosversionGet() != 0) return;

  if (R_FAILED(setsysInitialize())) return;
  SetSysFirmwareVersion firmware{};
  if (R_SUCCEEDED(setsysGetFirmwareVersion(&firmware)))
    hosversionSet(MAKEHOSVERSION(firmware.major, firmware.minor,
                                 firmware.micro));
  setsysExit();
}

}  // namespace

GameVolumeController::~GameVolumeController() { shutdown(); }

bool GameVolumeController::initialize(float volume) {
  if (initialized) return true;
  desiredVolume = std::clamp(volume, 0.0f, 1.0f);
  initializeHosVersion();

  Result result = pmdmntInitialize();
  if (R_FAILED(result)) {
    nxlog::pushf("Game volume unavailable: pmdmnt 0x%08X", result);
    return false;
  }

  result = audaInitialize();
  if (R_FAILED(result)) {
    pmdmntExit();
    nxlog::pushf("Game volume unavailable: aud:a 0x%08X", result);
    return false;
  }

  initialized = true;
  const auto now = Clock::now();
  nextProcessPoll = now;
  tick();
  nxlog::push("Game volume controller started");
  return true;
}

void GameVolumeController::shutdown() {
  if (!initialized) return;

  // Do not leave the running title attenuated after a normal service stop.
  restoreCurrent();
  audaExit();
  pmdmntExit();
  initialized = false;
  applicationPid = 0;
  nxlog::push("Game volume controller stopped; game volume restored");
}

bool GameVolumeController::restoreCurrent() {
  if (!initialized || applicationPid == 0) return initialized;
  const Result result =
      audaSetAudioOutputProcessMasterVolume(applicationPid, 0, 1.0f);
  return R_SUCCEEDED(result);
}

void GameVolumeController::setVolume(float volume) {
  desiredVolume = std::clamp(volume, 0.0f, 1.0f);
  if (!initialized || applicationPid == 0) return;
  apply();
}

void GameVolumeController::apply() {
  if (applicationPid == 0) return;
  const Result result = audaSetAudioOutputProcessMasterVolume(
      applicationPid, FadeDurationNs, desiredVolume);
  if (R_FAILED(result)) {
    if (!applyFailureLogged)
      nxlog::pushf("Could not apply game volume: 0x%08X", result);
    applyFailureLogged = true;
  } else if (applyFailureLogged) {
    nxlog::push("Game volume control recovered");
    applyFailureLogged = false;
  }
}

void GameVolumeController::tick() {
  if (!initialized) return;
  const auto now = Clock::now();

  if (now >= nextProcessPoll) {
    uint64_t pid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid))) pid = 0;
    if (pid != applicationPid) {
      applicationPid = pid;
      applyFailureLogged = false;
      if (applicationPid != 0) {
        nxlog::pushf("Game volume target PID: %llu",
                     static_cast<unsigned long long>(applicationPid));
        // A 100% target is already the Horizon default. Avoid touching the
        // audio service while Switify's audout session is active unless the
        // user actually requested attenuation.
        if (desiredVolume < 0.999f) apply();
      }
    }
    nextProcessPoll = now + ProcessPollInterval;
  }
}

}  // namespace switify::phase2
