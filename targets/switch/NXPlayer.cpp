#include "NXPlayer.h"

#include "NXConsoleLog.h"

#include <cstdint>      // for uint8_t
#include <chrono>
#include <functional>   // for __base
#include <iostream>     // for operator<<, basic_ostream, endl, cout
#include <memory>       // for shared_ptr, make_shared, make_unique
#include <mutex>        // for scoped_lock
#include <string_view>  // for hash, string_view
#include <type_traits>  // for remove_extent_t
#include <utility>      // for move
#include <variant>      // for get
#include <vector>       // for vector

#include "BellDSP.h"             // for BellDSP, BellDSP::FadeEffect, BellDS...
#include "BellUtils.h"           // for BELL_SLEEP_MS
#include "CentralAudioBuffer.h"  // for CentralAudioBuffer::AudioChunk, Cent...
#include "Logger.h"
#include "SpircHandler.h"  // for SpircHandler, SpircHandler::EventType
#include "StreamInfo.h"    // for BitWidth, BitWidth::BW_16
#include "TrackPlayer.h"   // for TrackPlayer

NXPlayer::NXPlayer(std::unique_ptr<AudioSink> sink,
                     std::shared_ptr<cspot::SpircHandler> handler)
    : bell::Task("player", 1024, 0, 0) {
  this->handler = handler;
  this->audioSink = std::move(sink);

  // Forty-eight decoded chunks provide about 1.1 seconds of PCM. Together
  // with audout's 0.4-second queue this absorbs short game/network stalls
  // without materially increasing the service footprint (+64 KiB).
  constexpr size_t DecodedPcmChunks = 48;
  this->centralAudioBuffer =
      std::make_shared<bell::CentralAudioBuffer>(DecodedPcmChunks);

#ifndef BELL_DISABLE_CODECS
  this->dsp = std::make_shared<bell::BellDSP>(this->centralAudioBuffer);
#endif

  this->handler->getTrackPlayer()->setDataCallback(
      [this](uint8_t* data, size_t bytes, std::string_view trackId) {
        auto hash = std::hash<std::string_view>{}(trackId);

        return this->centralAudioBuffer->writePCM(data, bytes, hash);
      });

  this->isPaused = false;

  this->handler->setEventHandler(
      [this](std::unique_ptr<cspot::SpircHandler::Event> event) {
        switch (event->eventType) {
          case cspot::SpircHandler::EventType::PLAY_PAUSE:
            if (std::get<bool>(event->data)) {
              this->pauseRequested = true;
            } else {
              this->isPaused = false;
              this->pauseRequested = false;
            }
            break;
          case cspot::SpircHandler::EventType::FLUSH: {
            this->centralAudioBuffer->clearBuffer();
            this->audioSink->flush();
            this->prebufferRequested = true;
            break;
          }
          case cspot::SpircHandler::EventType::DISC:
            this->centralAudioBuffer->clearBuffer();
            this->audioSink->flush();
            this->prebufferRequested = true;
            break;
          case cspot::SpircHandler::EventType::SEEK:
            this->centralAudioBuffer->clearBuffer();
            this->audioSink->flush();
            this->prebufferRequested = true;
            break;
          case cspot::SpircHandler::EventType::PLAYBACK_START:
            this->isPaused = true;
            this->playlistEnd = false;
            // A new local list/manual skip already emits FLUSH.  Do not clear
            // here for a natural track transition: the decoder may have
            // reached EOF while the final fraction of the previous song is
            // still queued for audout.
            break;
          case cspot::SpircHandler::EventType::DEPLETED:
            this->playlistEnd = true;
            break;
          case cspot::SpircHandler::EventType::VOLUME: {
            int volume = std::get<int>(event->data);
            break;
          }
          default:
            break;
        }
        if (this->uiEventSink)
          this->uiEventSink(event.get());
      });
  startTask();
}

void NXPlayer::feedData(uint8_t* data, size_t len) {}

void NXPlayer::runTask() {
  std::vector<uint8_t> outBuf = std::vector<uint8_t>(1024);

  std::scoped_lock lock(runningMutex);
  bell::CentralAudioBuffer::AudioChunk* chunk;

  size_t lastHash = 0;
  bool audioReached = false;
  bool underrunActive = false;
  auto underrunStarted = std::chrono::steady_clock::now();
  constexpr size_t PrebufferChunks = 12;

  while (isRunning) {
    if (!this->isPaused) {
      if (this->prebufferRequested.load() && !this->playlistEnd) {
        if (!this->centralAudioBuffer->hasAtLeast(PrebufferChunks)) {
          if (audioReached && !underrunActive) {
            underrunActive = true;
            underrunStarted = std::chrono::steady_clock::now();
          }
          BELL_SLEEP_MS(10);
          continue;
        }
        this->prebufferRequested = false;
      }
      chunk = this->centralAudioBuffer->readChunk();

      if (this->pauseRequested) {
        this->pauseRequested = false;
        nxlog::pushf("Pause requested");
#ifndef BELL_DISABLE_CODECS
        auto effect = std::make_unique<bell::BellDSP::FadeEffect>(
            44100 / 2, false, [this]() { this->isPaused = true; });
        this->dsp->queryInstantEffect(std::move(effect));
#else
        this->isPaused = true;
#endif
      }

      if (!chunk || chunk->pcmSize == 0) {
        if (this->playlistEnd) {
            this->handler->notifyAudioEnded();
            this->playlistEnd = false;
            audioReached = false;
            underrunActive = false;
        } else if (audioReached) {
            this->prebufferRequested = true;
            if (!underrunActive) {
              underrunActive = true;
              underrunStarted = std::chrono::steady_clock::now();
            }
        }
        BELL_SLEEP_MS(10);
        continue;
      } else {
        if (underrunActive) {
          const auto stalled =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - underrunStarted)
                  .count();
          if (stalled >= 50)
            nxlog::pushf("PCM underrun recovered after %lld ms",
                         static_cast<long long>(stalled));
          underrunActive = false;
        }
        if (lastHash != chunk->trackHash) {
          nxlog::pushf("Track change (hash %zu -> %zu)", lastHash,
                       (size_t)chunk->trackHash);
          lastHash = chunk->trackHash;
          audioReached = true;
          this->handler->notifyAudioReachedPlayback();
        }

#ifndef BELL_DISABLE_CODECS
        this->dsp->process(chunk->pcmData, chunk->pcmSize, 2, 44100,
                           bell::BitWidth::BW_16);
#endif
        this->audioSink->feedPCMFrames(chunk->pcmData, chunk->pcmSize);
      }
    } else {
      BELL_SLEEP_MS(10);
    }
  }
}

void NXPlayer::disconnect() {
  isRunning = false;
  std::scoped_lock lock(runningMutex);
}
