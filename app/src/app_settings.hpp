#pragma once

namespace swsettings {

struct Snapshot {
  bool playbackShortcuts = false;
  float outputVolume = 0.8f;
  bool highQuality = false;
  bool animatedPlayerBackground = false;
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

bool animatedPlayerBackgroundEnabled();
void setAnimatedPlayerBackgroundEnabled(bool enabled);

void setEqualizer(bool enabled, float bassDb, float midDb, float trebleDb);

}  // namespace swsettings
