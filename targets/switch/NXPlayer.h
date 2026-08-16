#pragma once

#include <stddef.h>  // for size_t
#include <stdint.h>  // for uint8_t
#include <atomic>    // for atomic
#include <memory>    // for shared_ptr, unique_ptr
#include <mutex>     // for mutex
#include <string>    // for string

#include <functional>

#include "AudioSink.h"  // for AudioSink
#include "BellTask.h"   // for Task
#include "SpircHandler.h"  // for SpircHandler::Event

namespace bell {
class BellDSP;
class CentralAudioBuffer;
}  // namespace bell
namespace cspot {
class SpircHandler;
}  // namespace cspot

class NXPlayer : public bell::Task {
 public:
  NXPlayer(std::unique_ptr<AudioSink> sink,
            std::shared_ptr<cspot::SpircHandler> spircHandler);
  void disconnect();

  // Optional: forward every SpircHandler event (after the player has
  // processed it) to a UI layer. Set once, before playback starts.
  void setUiEventSink(std::function<void(cspot::SpircHandler::Event*)> sink) {
    uiEventSink = std::move(sink);
  }

 private:
  std::string currentTrackId;
  std::shared_ptr<cspot::SpircHandler> handler;
  std::shared_ptr<bell::BellDSP> dsp;
  std::unique_ptr<AudioSink> audioSink;
  std::shared_ptr<bell::CentralAudioBuffer> centralAudioBuffer;

  void feedData(uint8_t* data, size_t len);

  std::atomic<bool> pauseRequested = false;
  std::atomic<bool> isPaused = true;
  std::atomic<bool> isRunning = true;
  std::mutex runningMutex;
  std::atomic<bool> playlistEnd = false;
  std::atomic<bool> prebufferRequested = true;
  std::function<void(cspot::SpircHandler::Event*)> uiEventSink;

  void runTask() override;
};
