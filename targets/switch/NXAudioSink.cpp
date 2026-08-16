#include "NXAudioSink.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "NXConsoleLog.h"

NXAudioSink::NXAudioSink() {
  initializeResampler();
  for (int i = 0; i < BUFFER_COUNT; i++) {
    // audout requires 0x1000-aligned buffers with 0x1000-aligned sizes
    size_t alignedSize = (BUFFER_BYTES + 0xFFF) & ~0xFFF;
    bufferMem[i] = aligned_alloc(0x1000, alignedSize);
    if (!bufferMem[i]) {
      nxlog::push("NXAudioSink: audio buffer allocation failed");
      for (int allocated = 0; allocated < i; ++allocated)
        free(bufferMem[allocated]);
      return;
    }
    memset(bufferMem[i], 0, alignedSize);

    audioBuffers[i].next = nullptr;
    audioBuffers[i].buffer = bufferMem[i];
    audioBuffers[i].buffer_size = alignedSize;
    audioBuffers[i].data_size = BUFFER_BYTES;
    audioBuffers[i].data_offset = 0;
    bufferQueued[i] = false;
  }

  prepared = true;
  nxlog::push("NXAudioSink: buffers prepared; audout deferred until playback");
}

NXAudioSink::~NXAudioSink() {
  if (initialized) {
    audoutStopAudioOut();
    audoutExit();
  }
  for (int i = 0; i < BUFFER_COUNT; i++) free(bufferMem[i]);
}

bool NXAudioSink::ensureInitialized() {
  if (initialized) return true;
  if (!prepared) return false;

  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    nxlog::pushf("NXAudioSink: lazy audoutInitialize failed: 0x%x", rc);
    return false;
  }

  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) {
    nxlog::pushf("NXAudioSink: lazy audoutStartAudioOut failed: 0x%x", rc);
    audoutExit();
    return false;
  }

  currentBuffer = 0;
  currentFill = 0;
  for (int i = 0; i < BUFFER_COUNT; ++i) bufferQueued[i] = false;
  initialized = true;
  nxlog::push("NXAudioSink: audout acquired for active playback");
  return true;
}

bool NXAudioSink::setParams(uint32_t sampleRate, uint8_t channelCount,
                            uint8_t bitDepth) {
  // Only 16-bit stereo input is supported; the sample rate feeds the
  // resampler ratio (cspot always delivers 44100).
  if (channelCount != 2 || bitDepth != 16 || sampleRate != 44100)
    return false;
  inputRate = sampleRate;
  resetRequested = true;
  return true;
}

void NXAudioSink::flush() { resetRequested = true; }

void NXAudioSink::setVolume(float value) {
  volume = std::clamp(value, 0.0f, 1.0f);
}

void NXAudioSink::setEqualizer(bool enabled, float bassDb, float midDb,
                               float trebleDb) {
  equalizerRequested = enabled;
  bassDbRequested = std::clamp(bassDb, -12.0f, 12.0f);
  midDbRequested = std::clamp(midDb, -12.0f, 12.0f);
  trebleDbRequested = std::clamp(trebleDb, -12.0f, 12.0f);
  equalizerGeneration.fetch_add(1);
}

float NXAudioSink::Biquad::process(float sample, int channel) {
  const double output = b0 * sample + z1[channel];
  z1[channel] = b1 * sample - a1 * output + z2[channel];
  z2[channel] = b2 * sample - a2 * output;
  return static_cast<float>(output);
}

void NXAudioSink::Biquad::reset() {
  z1[0] = z1[1] = 0.0;
  z2[0] = z2[1] = 0.0;
}

namespace {
constexpr double kPi = 3.14159265358979323846;

void normalize(NXAudioSink::Biquad& filter, double b0, double b1, double b2,
               double a0, double a1, double a2) {
  filter.b0 = b0 / a0;
  filter.b1 = b1 / a0;
  filter.b2 = b2 / a0;
  filter.a1 = a1 / a0;
  filter.a2 = a2 / a0;
  filter.reset();
}

void configurePeak(NXAudioSink::Biquad& filter, double sampleRate,
                   double frequency, double q, double gainDb) {
  const double a = std::pow(10.0, gainDb / 40.0);
  const double omega = 2.0 * kPi * frequency / sampleRate;
  const double alpha = std::sin(omega) / (2.0 * q);
  const double cosine = std::cos(omega);
  normalize(filter, 1.0 + alpha * a, -2.0 * cosine,
            1.0 - alpha * a, 1.0 + alpha / a, -2.0 * cosine,
            1.0 - alpha / a);
}

void configureShelf(NXAudioSink::Biquad& filter, double sampleRate,
                    double frequency, double gainDb, bool high) {
  const double a = std::pow(10.0, gainDb / 40.0);
  const double omega = 2.0 * kPi * frequency / sampleRate;
  const double cosine = std::cos(omega);
  const double sine = std::sin(omega);
  const double alpha = sine / std::sqrt(2.0);
  const double rootAAlpha = 2.0 * std::sqrt(a) * alpha;
  if (high) {
    normalize(filter,
              a * ((a + 1.0) + (a - 1.0) * cosine + rootAAlpha),
              -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine),
              a * ((a + 1.0) + (a - 1.0) * cosine - rootAAlpha),
              (a + 1.0) - (a - 1.0) * cosine + rootAAlpha,
              2.0 * ((a - 1.0) - (a + 1.0) * cosine),
              (a + 1.0) - (a - 1.0) * cosine - rootAAlpha);
  } else {
    normalize(filter,
              a * ((a + 1.0) - (a - 1.0) * cosine + rootAAlpha),
              2.0 * a * ((a - 1.0) - (a + 1.0) * cosine),
              a * ((a + 1.0) - (a - 1.0) * cosine - rootAAlpha),
              (a + 1.0) + (a - 1.0) * cosine + rootAAlpha,
              -2.0 * ((a - 1.0) + (a + 1.0) * cosine),
              (a + 1.0) + (a - 1.0) * cosine - rootAAlpha);
  }
}
}  // namespace

void NXAudioSink::updateEqualizer() {
  const uint32_t generation = equalizerGeneration.load();
  if (generation == appliedEqualizerGeneration) return;
  appliedEqualizerGeneration = generation;
  equalizerActive = equalizerRequested.load();
  const float bass = bassDbRequested.load();
  const float mid = midDbRequested.load();
  const float treble = trebleDbRequested.load();
  configureShelf(bassFilter, 48000.0, 120.0, bass, false);
  configurePeak(midFilter, 48000.0, 1000.0, 0.8, mid);
  configureShelf(trebleFilter, 48000.0, 8000.0, treble, true);
  const float positiveBoost = std::max(0.0f, bass) + std::max(0.0f, mid) +
                              std::max(0.0f, treble);
  equalizerHeadroom = std::pow(10.0f, -positiveBoost / 20.0f);
}

void NXAudioSink::submitCurrentBuffer() {
  audioBuffers[currentBuffer].data_size = currentFill;
  const Result appendResult =
      audoutAppendAudioOutBuffer(&audioBuffers[currentBuffer]);
  if (R_FAILED(appendResult)) {
    nxlog::pushf("NXAudioSink: audout append failed: 0x%x", appendResult);
    bufferQueued[currentBuffer] = false;
  } else {
    bufferQueued[currentBuffer] = true;
  }
  currentFill = 0;
  currentBuffer = (currentBuffer + 1) % BUFFER_COUNT;

  // If the next buffer is still queued in audout, block until a buffer is
  // released. This is the backpressure that paces the whole player.
  while (bufferQueued[currentBuffer]) {
    AudioOutBuffer* released = nullptr;
    u32 releasedCount = 0;
    Result rc = audoutWaitPlayFinish(&released, &releasedCount, UINT64_MAX);
    if (R_FAILED(rc))
      break;
    for (u32 n = 0; n < releasedCount && released != nullptr; n++) {
      for (int i = 0; i < BUFFER_COUNT; i++) {
        if (released == &audioBuffers[i]) {
          bufferQueued[i] = false;
          break;
        }
      }
      released = (AudioOutBuffer*)released->next;
    }
  }
}

void NXAudioSink::pushOutputFrame(float left, float right) {
  updateEqualizer();
  if (equalizerActive) {
    left = bassFilter.process(left, 0);
    left = midFilter.process(left, 0);
    left = trebleFilter.process(left, 0);
    right = bassFilter.process(right, 1);
    right = midFilter.process(right, 1);
    right = trebleFilter.process(right, 1);
    left *= equalizerHeadroom;
    right *= equalizerHeadroom;
  }
  const float gain = volume.load();
  const int16_t l = static_cast<int16_t>(std::lrintf(
      std::clamp(left * gain, -32768.0f, 32767.0f)));
  const int16_t r = static_cast<int16_t>(std::lrintf(
      std::clamp(right * gain, -32768.0f, 32767.0f)));
  int16_t* out = (int16_t*)((uint8_t*)bufferMem[currentBuffer] + currentFill);
  out[0] = l;
  out[1] = r;
  currentFill += BYTES_PER_FRAME;
  if (currentFill >= BUFFER_BYTES) {
    submitCurrentBuffer();
  }
}

void NXAudioSink::initializeResampler() {
  constexpr double cutoff = 0.94;
  constexpr double radius = RESAMPLER_TAPS / 2.0;
  for (size_t phase = 0; phase < RESAMPLER_PHASES; ++phase) {
    const double fraction =
        static_cast<double>(phase) / RESAMPLER_PHASES;
    double sum = 0.0;
    for (size_t tap = 0; tap < RESAMPLER_TAPS; ++tap) {
      const double offset = static_cast<double>(tap) - 7.0;
      const double distance = offset - fraction;
      double coefficient = 0.0;
      if (std::abs(distance) < radius) {
        const double sinc =
            std::abs(distance) < 1.0e-9
                ? cutoff
                : std::sin(kPi * cutoff * distance) /
                      (kPi * distance);
        const double window =
            0.42 + 0.5 * std::cos(kPi * distance / radius) +
            0.08 * std::cos(2.0 * kPi * distance / radius);
        coefficient = sinc * window;
      }
      resamplerCoefficients[phase][tap] =
          static_cast<float>(coefficient);
      sum += coefficient;
    }
    if (std::abs(sum) > 1.0e-9)
      for (auto& coefficient : resamplerCoefficients[phase])
        coefficient = static_cast<float>(coefficient / sum);
  }
}

void NXAudioSink::resetResampler() {
  resamplerHead = 0;
  resamplerCount = 0;
  resamplerStartFrame = 0;
  resamplerInputFrames = 0;
  nextOutputNumerator = 0;
  firstInputFrame = {};
  hasFirstInputFrame = false;
  currentFill = 0;
  bassFilter.reset();
  midFilter.reset();
  trebleFilter.reset();
}

NXAudioSink::InputFrame NXAudioSink::inputFrameAt(int64_t frame) const {
  if (frame < 0) return firstInputFrame;
  const uint64_t absolute = static_cast<uint64_t>(frame);
  if (absolute < resamplerStartFrame || resamplerCount == 0)
    return resamplerRing[resamplerHead];
  const uint64_t offset = absolute - resamplerStartFrame;
  if (offset >= resamplerCount) {
    const size_t tail =
        (resamplerHead + resamplerCount - 1) % RESAMPLER_RING_FRAMES;
    return resamplerRing[tail];
  }
  return resamplerRing[(resamplerHead + static_cast<size_t>(offset)) %
                       RESAMPLER_RING_FRAMES];
}

void NXAudioSink::produceResampledFrames() {
  constexpr uint64_t outputRate = 48000;
  constexpr uint64_t phaseStride = outputRate / RESAMPLER_PHASES;
  while (resamplerCount != 0) {
    const uint64_t base = nextOutputNumerator / outputRate;
    if (base + 8 >= resamplerInputFrames) break;
    const size_t phase = static_cast<size_t>(
        (nextOutputNumerator % outputRate) / phaseStride);
    float left = 0.0f;
    float right = 0.0f;
    for (size_t tap = 0; tap < RESAMPLER_TAPS; ++tap) {
      const auto input =
          inputFrameAt(static_cast<int64_t>(base) +
                       static_cast<int64_t>(tap) - 7);
      const float coefficient = resamplerCoefficients[phase][tap];
      left += input.left * coefficient;
      right += input.right * coefficient;
    }
    pushOutputFrame(left, right);
    nextOutputNumerator += inputRate;

    const uint64_t nextBase = nextOutputNumerator / outputRate;
    const uint64_t keepFrom = nextBase > 8 ? nextBase - 8 : 0;
    while (resamplerCount != 0 && resamplerStartFrame < keepFrom) {
      resamplerHead = (resamplerHead + 1) % RESAMPLER_RING_FRAMES;
      --resamplerCount;
      ++resamplerStartFrame;
    }
  }
}

void NXAudioSink::appendInputFrame(float left, float right) {
  const InputFrame frame{left, right};
  if (!hasFirstInputFrame) {
    firstInputFrame = frame;
    hasFirstInputFrame = true;
  }
  if (resamplerCount == RESAMPLER_RING_FRAMES) {
    resamplerHead = (resamplerHead + 1) % RESAMPLER_RING_FRAMES;
    --resamplerCount;
    ++resamplerStartFrame;
  }
  const size_t tail =
      (resamplerHead + resamplerCount) % RESAMPLER_RING_FRAMES;
  resamplerRing[tail] = frame;
  ++resamplerCount;
  ++resamplerInputFrames;
  produceResampledFrames();
}

void NXAudioSink::feedPCMFrames(const uint8_t* buffer, size_t bytes) {
  if (bytes < BYTES_PER_FRAME || !ensureInitialized())
    return;

  if (resetRequested.exchange(false)) resetResampler();

  const int16_t* in = (const int16_t*)buffer;
  const size_t inFrames = bytes / BYTES_PER_FRAME;
  for (size_t frame = 0; frame < inFrames; ++frame)
    appendInputFrame(static_cast<float>(in[frame * 2]),
                     static_cast<float>(in[frame * 2 + 1]));
}
