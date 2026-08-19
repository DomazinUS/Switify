#pragma once

namespace swsettings {

enum class PlayerBackgroundMode { Off = 0, Gradient = 1, Solid = 2 };

struct Snapshot {
  bool playbackShortcuts = false;
  float outputVolume = 0.8f;
  bool highQuality = false;
  PlayerBackgroundMode playerBackgroundMode = PlayerBackgroundMode::Solid;
  bool showSyncedLyricsInFullPlayer = true;
  bool equalizerEnabled = false;
  float bassDb = 0.0f;
  float midDb = 0.0f;
  float trebleDb = 0.0f;
};

void load();
Snapshot get();

bool playbackShortcutsEnabled();
void setPlaybackShortcutsEnabled(bool enabled);

void setOutputVolume(float volume);
void setHighQuality(bool enabled);

PlayerBackgroundMode playerBackgroundMode();
void setPlayerBackgroundMode(PlayerBackgroundMode mode);

bool showSyncedLyricsInFullPlayer();
void setShowSyncedLyricsInFullPlayer(bool enabled);

void setEqualizer(bool enabled, float bassDb, float midDb, float trebleDb);

}  // namespace swsettings
