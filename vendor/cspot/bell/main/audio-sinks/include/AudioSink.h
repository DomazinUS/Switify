#ifndef AUDIOSINK_H
#define AUDIOSINK_H

#include <cstdint>
#include <cstdlib>
#include <vector>

class AudioSink {
 public:
  AudioSink() {}
  virtual ~AudioSink() {}
  virtual void feedPCMFrames(const uint8_t* buffer, size_t bytes) = 0;
  // Discard any partial conversion state after a seek or explicit stream
  // flush. Hardware sinks may defer the reset to their audio thread.
  virtual void flush() {}
  virtual void volumeChanged(uint16_t volume) {}
  // Return false if the sink doesn't support reconfiguration.
  virtual bool setParams(uint32_t sampleRate, uint8_t channelCount,
                         uint8_t bitDepth) {
    return false;
  }
  // Deprecated. Implement/use setParams() instead.
  virtual inline bool setRate(uint16_t sampleRate) {
    return setParams(sampleRate, 2, 16);
  }
  bool softwareVolumeControl = true;
  bool usign = false;
};

#endif
