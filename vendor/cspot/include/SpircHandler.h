#pragma once

#include <atomic>
#include <stdint.h>    // for uint32_t, uint8_t
#include <functional>  // for function
#include <memory>      // for shared_ptr, unique_ptr
#include <string>      // for string
#include <variant>     // for variant
#include <vector>      // for vector

#include "CDNAudioFile.h"  // for CDNTrackStream, CDNTrackStream::Track...
#include "TrackQueue.h"
#include "protobuf/spirc.pb.h"  // for MessageType

namespace cspot {
class TrackPlayer;
struct Context;

class SpircHandler {
 public:
  enum class LocalRepeatMode : uint8_t { Off = 0, All = 1, One = 2 };

  // Legacy SPIRC remote control is optional. Spotify is retiring the
  // hm://remote/user subscription, while Switify drives its queue locally.
  SpircHandler(std::shared_ptr<cspot::Context> ctx,
               bool enableRemoteControl = true);

  enum class EventType {
    PLAY_PAUSE,
    VOLUME,
    TRACK_INFO,
    DISC,
    NEXT,
    PREV,
    SEEK,
    DEPLETED,
    FLUSH,
    PLAYBACK_START
  };

  typedef std::variant<TrackInfo, int, bool> EventData;

  struct Event {
    EventType eventType;
    EventData data;
  };

  typedef std::function<void(std::unique_ptr<Event>)> EventHandler;

  void subscribeToMercury();
  std::shared_ptr<TrackPlayer> getTrackPlayer();

  void setEventHandler(EventHandler handler);

  void setPause(bool pause);

  bool previousSong();

  bool nextSong();

  void notifyAudioReachedPlayback();
  void notifyAudioEnded();
  void updatePositionMs(uint32_t position);
  void setRemoteVolume(int volume);
  void loadTrackFromURI(const std::string& uri);

  // Switify extension: start playback of a track list LOCALLY, without a
  // remote Connect device. Mirrors the kMessageTypeLoad path, but the track
  // list comes from this device instead of a remote frame.
  // uris: spotify:track:<base62 id> entries; index: track to start at.
  void playLocalTrackList(const std::vector<std::string>& uris, size_t index,
                          uint32_t positionMs = 0);
  // Change the current local queue without restarting its audible stream.
  bool replaceLocalTrackList(const std::vector<std::string>& uris,
                             size_t currentIndex);
  void setLocalRepeatMode(LocalRepeatMode mode) { localRepeatMode = mode; }
  std::shared_ptr<cspot::TrackQueue> getTrackQueue() { return trackQueue; }

  void disconnect();

 private:
  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::TrackPlayer> trackPlayer;
  std::shared_ptr<cspot::TrackQueue> trackQueue;

  EventHandler eventHandler = nullptr;

  std::shared_ptr<cspot::PlaybackState> playbackState;
  bool remoteControlEnabled = true;
  std::atomic<LocalRepeatMode> localRepeatMode{LocalRepeatMode::Off};

  void sendCmd(MessageType typ);

  void sendEvent(EventType type);
  void sendEvent(EventType type, EventData data);

  bool skipSong(TrackQueue::SkipDirection dir);
  void handleFrame(std::vector<uint8_t>& data);
  void notify();
};
}  // namespace cspot
