#include "app_settings.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>

#ifdef __SWITCH__
#include <sys/stat.h>
#endif

#include "NXConsoleLog.h"
#include "nlohmann/json.hpp"

namespace swsettings {
namespace {
#ifdef __SWITCH__
constexpr const char* kSettingsFile = "/config/Switify/settings.json";
#else
constexpr const char* kSettingsFile = "settings.json";
#endif

std::atomic_bool gPlaybackShortcuts{false};
std::atomic<float> gOutputVolume{0.8f};
std::atomic_bool gHighQuality{false};
std::atomic_bool gAnimatedPlayerBackground{false};
std::atomic_bool gEqualizerEnabled{false};
std::atomic<float> gBassDb{0.0f};
std::atomic<float> gMidDb{0.0f};
std::atomic<float> gTrebleDb{0.0f};
std::mutex gSaveMutex;

float clampDb(float value) { return std::clamp(value, -12.0f, 12.0f); }
float clampVolume(float value) { return std::clamp(value, 0.0f, 1.0f); }

void save() {
  std::lock_guard<std::mutex> lock(gSaveMutex);
#ifdef __SWITCH__
  mkdir("/config", 0755);
  mkdir("/config/Switify", 0755);
#endif
  const auto snapshot = get();
  nlohmann::json json = {
      {"version", 3},
      {"playback_shortcuts", snapshot.playbackShortcuts},
      {"output_volume", snapshot.outputVolume},
      {"audio_quality", snapshot.highQuality ? "high" : "normal"},
      {"animated_player_background", snapshot.animatedPlayerBackground},
      {"equalizer_enabled", snapshot.equalizerEnabled},
      {"bass_db", snapshot.bassDb},
      {"mid_db", snapshot.midDb},
      {"treble_db", snapshot.trebleDb},
  };
  std::ofstream file(kSettingsFile, std::ios::trunc);
  if (file.good()) file << json.dump(2);
}
}  // namespace

void load() {
  try {
    std::ifstream file(kSettingsFile);
    if (!file.good()) return;
    nlohmann::json json;
    file >> json;
    gPlaybackShortcuts = json.value("playback_shortcuts", false);
    gOutputVolume = clampVolume(json.value("output_volume", 0.8f));
    gHighQuality = json.value("audio_quality", std::string("normal")) ==
                   "high";
    gAnimatedPlayerBackground =
        json.value("animated_player_background", false);
    gEqualizerEnabled = json.value("equalizer_enabled", false);
    gBassDb = clampDb(json.value("bass_db", 0.0f));
    gMidDb = clampDb(json.value("mid_db", 0.0f));
    gTrebleDb = clampDb(json.value("treble_db", 0.0f));
  } catch (const std::exception& error) {
    nxlog::pushf("Ignoring invalid settings file: %s", error.what());
  }
}

Snapshot get() {
  Snapshot snapshot;
  snapshot.playbackShortcuts = gPlaybackShortcuts.load();
  snapshot.outputVolume = gOutputVolume.load();
  snapshot.highQuality = gHighQuality.load();
  snapshot.animatedPlayerBackground = gAnimatedPlayerBackground.load();
  snapshot.equalizerEnabled = gEqualizerEnabled.load();
  snapshot.bassDb = gBassDb.load();
  snapshot.midDb = gMidDb.load();
  snapshot.trebleDb = gTrebleDb.load();
  return snapshot;
}

bool playbackShortcutsEnabled() { return gPlaybackShortcuts.load(); }

void setPlaybackShortcutsEnabled(bool enabled) {
  gPlaybackShortcuts = enabled;
  save();
}

void setOutputVolume(float volume) {
  gOutputVolume = clampVolume(volume);
  save();
}

void setHighQuality(bool enabled) {
  gHighQuality = enabled;
  save();
}

bool animatedPlayerBackgroundEnabled() {
  return gAnimatedPlayerBackground.load();
}

void setAnimatedPlayerBackgroundEnabled(bool enabled) {
  gAnimatedPlayerBackground = enabled;
  save();
}

void setEqualizer(bool enabled, float bassDb, float midDb, float trebleDb) {
  gEqualizerEnabled = enabled;
  gBassDb = clampDb(bassDb);
  gMidDb = clampDb(midDb);
  gTrebleDb = clampDb(trebleDb);
  save();
}

}  // namespace swsettings
