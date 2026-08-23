#pragma once

#include <chrono>
#include <cstdint>

namespace switify::phase2 {

// Owns the single aud:a session used to attenuate the foreground application.
// The Spotify stream is not affected; it keeps using Switify's software gain.
class GameVolumeController {
 public:
  GameVolumeController() = default;
  ~GameVolumeController();

  bool initialize(float volume);
  void shutdown();
  void tick();
  void setVolume(float volume);
  bool restoreCurrent();

  bool ready() const { return initialized; }
  bool gameRunning() const { return applicationPid != 0; }
  float volume() const { return desiredVolume; }

 private:
  using Clock = std::chrono::steady_clock;

  void apply();

  bool initialized = false;
  bool applyFailureLogged = false;
  uint64_t applicationPid = 0;
  float desiredVolume = 1.0f;
  Clock::time_point nextProcessPoll{};
};

}  // namespace switify::phase2
