#pragma once

// audout-based AudioSink for Nintendo Switch (libnx).
//
// audout runs at a fixed 48000 Hz; cspot delivers 44100 Hz s16 stereo. A
// compact 16-tap, 160-phase windowed-sinc converter performs the fixed
// 147:160 conversion without the high-frequency roughness of the original
// linear POC resampler.
//
// feedPCMFrames() blocks when all audout buffers are queued, which provides
// natural backpressure to the player thread (same behavior as the ALSA sink).

#include <switch.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdlib>

#include "AudioSink.h"

class NXAudioSink : public AudioSink {
 public:
  NXAudioSink();
  ~NXAudioSink() override;

  void feedPCMFrames(const uint8_t* buffer, size_t bytes) override;
  void flush() override;
  bool setParams(uint32_t sampleRate, uint8_t channelCount,
                 uint8_t bitDepth) override;

  // Software gain applied before samples enter audout.  audout itself does
  // not expose a per-application volume control, so the UI drives this value
  // while SpircHandler mirrors it to other Spotify Connect clients.
  void setVolume(float value);
  float getVolume() const { return volume.load(); }
  void setEqualizer(bool enabled, float bassDb, float midDb, float trebleDb);

  // Buffer preparation is enough for construction to succeed. audout is
  // acquired lazily by the first PCM frame, so a restored-paused session does
  // not reserve an audio endpoint merely because Spotify connected.
  bool ok() const { return prepared; }

 private:
  static constexpr int BUFFER_COUNT = 4;
  // 4800 output frames per buffer = 100 ms at 48 kHz, s16 stereo
  static constexpr size_t FRAMES_PER_BUFFER = 4800;
  static constexpr size_t BYTES_PER_FRAME = 4;  // s16 * 2 channels
  static constexpr size_t BUFFER_BYTES = FRAMES_PER_BUFFER * BYTES_PER_FRAME;

  AudioOutBuffer audioBuffers[BUFFER_COUNT];
  void* bufferMem[BUFFER_COUNT]{};
  bool bufferQueued[BUFFER_COUNT]{};

  int currentBuffer = 0;      // buffer currently being filled
  size_t currentFill = 0;     // bytes written into the current buffer
  bool prepared = false;
  bool initialized = false;
  std::atomic<float> volume{0.8f};

 public:
  // Exposed only so the coefficient helpers in the implementation can remain
  // small; filter instances themselves stay private.
  struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
    double z1[2]{};
    double z2[2]{};

    float process(float sample, int channel);
    void reset();
  };

 private:
  Biquad bassFilter;
  Biquad midFilter;
  Biquad trebleFilter;
  std::atomic<bool> equalizerRequested{false};
  std::atomic<float> bassDbRequested{0.0f};
  std::atomic<float> midDbRequested{0.0f};
  std::atomic<float> trebleDbRequested{0.0f};
  std::atomic<uint32_t> equalizerGeneration{1};
  uint32_t appliedEqualizerGeneration = 0;
  bool equalizerActive = false;
  float equalizerHeadroom = 1.0f;

  // Fixed-ratio streaming resampler state (44100 -> 48000).
  static constexpr size_t RESAMPLER_TAPS = 16;
  static constexpr size_t RESAMPLER_PHASES = 160;
  static constexpr size_t RESAMPLER_RING_FRAMES = 64;
  struct InputFrame {
    float left = 0.0f;
    float right = 0.0f;
  };
  std::array<std::array<float, RESAMPLER_TAPS>, RESAMPLER_PHASES>
      resamplerCoefficients{};
  std::array<InputFrame, RESAMPLER_RING_FRAMES> resamplerRing{};
  size_t resamplerHead = 0;
  size_t resamplerCount = 0;
  uint64_t resamplerStartFrame = 0;
  uint64_t resamplerInputFrames = 0;
  uint64_t nextOutputNumerator = 0;
  InputFrame firstInputFrame{};
  bool hasFirstInputFrame = false;
  std::atomic<bool> resetRequested{true};
  uint32_t inputRate = 44100;

  bool ensureInitialized();
  void submitCurrentBuffer();
  int16_t* acquireWritePointer();  // blocks until the current buffer has room
  void pushOutputFrame(float l, float r);
  void updateEqualizer();
  void initializeResampler();
  void resetResampler();
  void appendInputFrame(float left, float right);
  InputFrame inputFrameAt(int64_t frame) const;
  void produceResampledFrames();
};
