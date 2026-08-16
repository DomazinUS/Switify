#include "TrackPlayer.h"

#include <exception>    // for exception
#include <mutex>        // for mutex, scoped_lock
#include <string>       // for string
#include <type_traits>  // for remove_extent_t
#include <vector>       // for vector, vector<>::value_type

#include "BellLogger.h"        // for AbstractLogger
#include "BellUtils.h"         // for BELL_SLEEP_MS
#include "Logger.h"            // for CSPOT_LOG
#include "Packet.h"            // for cspot
#include "TrackQueue.h"        // for CDNTrackStream, CDNTrackStream::TrackInfo
#include "WrappedSemaphore.h"  // for WrappedSemaphore

#ifdef BELL_VORBIS_FLOAT
#define VORBIS_SEEK(file, position) \
  (ov_time_seek(file, (double)position / 1000))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, 0, 2, 1, section))
#else
#define VORBIS_SEEK(file, position) (ov_time_seek(file, position))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, section))
#endif

namespace cspot {
struct Context;
struct TrackReference;
}  // namespace cspot

using namespace cspot;

static size_t vorbisReadCb(void* ptr, size_t size, size_t nmemb,
                           TrackPlayer* self) {
  return self->_vorbisRead(ptr, size, nmemb);
}

static int vorbisCloseCb(TrackPlayer* self) {
  return self->_vorbisClose();
}

static int vorbisSeekCb(TrackPlayer* self, int64_t offset, int whence) {

  return self->_vorbisSeek(offset, whence);
}

static long vorbisTellCb(TrackPlayer* self) {
  return self->_vorbisTell();
}

TrackPlayer::TrackPlayer(std::shared_ptr<cspot::Context> ctx,
                         std::shared_ptr<cspot::TrackQueue> trackQueue,
                         EOFCallback eof, TrackLoadedCallback trackLoaded)
    : bell::Task("cspot_player", 48 * 1024, 5, 1) {
  this->ctx = ctx;
  this->eofCallback = eof;
  this->trackLoaded = trackLoaded;
  this->trackQueue = trackQueue;
  this->playbackSemaphore = std::make_unique<bell::WrappedSemaphore>(5);

  // Initialize vorbis callbacks
  vorbisFile = {};
  vorbisCallbacks = {
      (decltype(ov_callbacks::read_func))&vorbisReadCb,
      (decltype(ov_callbacks::seek_func))&vorbisSeekCb,
      (decltype(ov_callbacks::close_func))&vorbisCloseCb,
      (decltype(ov_callbacks::tell_func))&vorbisTellCb,
  };
}

TrackPlayer::~TrackPlayer() {
  isRunning = false;
  resetState();
  std::scoped_lock lock(runningMutex);
}

void TrackPlayer::start() {
  if (!isRunning) {
    isRunning = true;
    startTask();
  }
}

void TrackPlayer::stop() {
  isRunning = false;
  resetState();
  std::scoped_lock lock(runningMutex);
}

void TrackPlayer::resetState(bool paused) {
  // Mark for reset
  this->pendingReset = true;
  this->currentSongPlaying = false;
  this->startPaused = paused;

  std::scoped_lock lock(dataOutMutex);

  CSPOT_LOG(info, "Resetting state");
}

void TrackPlayer::seekMs(size_t ms) {
  if (inFuture) {
    // We're in the middle of the next track, so we need to reset the player in order to seek
    resetState();
  }

  CSPOT_LOG(info, "Seeking...");
  this->pendingSeekPositionMs = ms;
}

void TrackPlayer::runTask() {
  std::scoped_lock lock(runningMutex);

  std::shared_ptr<QueuedTrack> track, newTrack = nullptr;

  int trackOffset = 0;
  bool eof = false;
  bool endOfQueueReached = false;

  while (isRunning) {
    // Ensure we even have any tracks to play
    if (!this->trackQueue->hasTracks() ||
        (!pendingReset && endOfQueueReached && trackQueue->isFinished())) {
      this->trackQueue->playableSemaphore->twait(300);
      continue;
    }

    // Last track was interrupted, reset to default
    if (pendingReset) {
      track = nullptr;
      pendingReset = false;
      inFuture = false;
    }

    endOfQueueReached = false;

    // Wait 800ms. If next reset is requested in meantime, restart the queue.
    // Gets rid of excess actions during rapid queueing
    BELL_SLEEP_MS(50);

    if (pendingReset) {
      continue;
    }

    newTrack = trackQueue->consumeTrack(track, trackOffset);

    if (newTrack == nullptr) {
      if (trackOffset == -1) {
        // Reset required
        track = nullptr;
      }

      BELL_SLEEP_MS(100);
      continue;
    }

    track = newTrack;

    inFuture = trackOffset > 0;

    if (track->state != QueuedTrack::State::READY) {
      int waitedMs = 0;
      while (track->state != QueuedTrack::State::READY &&
             track->state != QueuedTrack::State::FAILED && isRunning &&
             !pendingReset && waitedMs < 20000) {
        track->loadedSemaphore->twait(500);
        waitedMs += 500;
      }

      if (track->state != QueuedTrack::State::READY) {
        CSPOT_LOG(error, "Track failed to load after %d ms, skipping it",
                  waitedMs);
        this->eofCallback();
        continue;
      }
    }

    CSPOT_LOG(info, "Got track ID=%s", track->identifier.c_str());

    currentSongPlaying = true;

    {
      std::scoped_lock lock(playbackMutex);

      currentTrackStream = track->getAudioFile();

      // DNS can briefly disappear while Horizon changes display/power state.
      // Opening a CDN stream used to let that exception escape this worker
      // thread, which invokes std::terminate and kills the whole application.
      // Retry the same song, then fail it gracefully if connectivity does not
      // return instead of crashing Switify.
      bool streamOpened = false;
      for (int attempt = 1;
           attempt <= 4 && isRunning && !pendingReset && currentSongPlaying;
           ++attempt) {
        try {
          currentTrackStream = track->getAudioFile();
          currentTrackStream->openStream();
          streamOpened = true;
          if (attempt > 1)
            CSPOT_LOG(info, "Audio stream recovered on attempt %d", attempt);
          break;
        } catch (const std::exception& error) {
          CSPOT_LOG(error, "Audio stream open attempt %d/4 failed: %s",
                    attempt, error.what());
        } catch (...) {
          CSPOT_LOG(error, "Audio stream open attempt %d/4 failed", attempt);
        }

        currentTrackStream = nullptr;
        if (attempt < 4 && isRunning && !pendingReset)
          BELL_SLEEP_MS(750);
      }

      if (!streamOpened) {
        CSPOT_LOG(error,
                  "Audio stream unavailable after retries; skipping track");
        currentSongPlaying = false;
        track->loading = false;
        currentTrackStream = nullptr;
        continue;
      }

      if (pendingReset || !currentSongPlaying) {
        continue;
      }

      if (pendingSeekPositionMs > 0) {
        track->requestedPosition = pendingSeekPositionMs;
      }

      // Announce the exact stream that successfully opened. Queue position
      // used to be inferred later from audio-buffer transitions, which made
      // the UI show the previous song whenever a slow entry was skipped.
      this->trackLoaded(track, startPaused);
      startPaused = false;

      int32_t r =
          ov_open_callbacks(this, &vorbisFile, NULL, 0, vorbisCallbacks);

      if (track->requestedPosition > 0) {
        VORBIS_SEEK(&vorbisFile, track->requestedPosition);
      }

      eof = false;
      track->loading = true;

      CSPOT_LOG(info, "Playing");

      while (!eof && currentSongPlaying) {
        // Execute seek if needed
        if (pendingSeekPositionMs > 0) {
          uint32_t seekPosition = pendingSeekPositionMs;

          // Reset the pending seek position
          pendingSeekPositionMs = 0;

          // Seek to the new position
          VORBIS_SEEK(&vorbisFile, seekPosition);
        }

        long ret = VORBIS_READ(&vorbisFile, (char*)&pcmBuffer[0],
                               pcmBuffer.size(), &currentSection);

        if (ret == 0) {
          CSPOT_LOG(info, "EOF");
          // and done :)
          eof = true;
        } else if (ret < 0) {
          CSPOT_LOG(error, "An error has occured in the stream %d", ret);
          currentSongPlaying = false;
        } else {
          if (this->dataCallback != nullptr) {
            auto toWrite = ret;

            while (!eof && currentSongPlaying && !pendingReset && toWrite > 0) {
              int written = 0;
              {
                std::scoped_lock dataOutLock(dataOutMutex);
                // If reset happened during playback, return
                if (!currentSongPlaying || pendingReset)
                  break;

                written = dataCallback(pcmBuffer.data() + (ret - toWrite),
                                       toWrite, track->identifier);
              }
              if (written == 0) {
                BELL_SLEEP_MS(50);
              }
              toWrite -= written;
            }
          }
        }
      }
      ov_clear(&vorbisFile);

      CSPOT_LOG(info, "Playing done");

      // always move back to LOADING (ensure proper seeking after last track has been loaded)
      currentTrackStream = nullptr;
      track->loading = false;
    }

    if (eof) {
      const bool restarted = this->eofCallback();
      if (restarted) {
        track = nullptr;
        endOfQueueReached = false;
      } else if (trackQueue->isFinished()) {
        endOfQueueReached = true;
      }
    }
  }
}

size_t TrackPlayer::_vorbisRead(void* ptr, size_t size, size_t nmemb) {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  return this->currentTrackStream->readBytes((uint8_t*)ptr, nmemb * size);
}

size_t TrackPlayer::_vorbisClose() {
  return 0;
}

int TrackPlayer::_vorbisSeek(int64_t offset, int whence) {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  switch (whence) {
    case 0:
      this->currentTrackStream->seek(offset);  // Spotify header offset
      break;
    case 1:
      this->currentTrackStream->seek(this->currentTrackStream->getPosition() +
                                     offset);
      break;
    case 2:
      this->currentTrackStream->seek(this->currentTrackStream->getSize() +
                                     offset);
      break;
  }

  return 0;
}

long TrackPlayer::_vorbisTell() {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  return this->currentTrackStream->getPosition();
}

void TrackPlayer::setDataCallback(DataCallback callback) {
  this->dataCallback = callback;
}
