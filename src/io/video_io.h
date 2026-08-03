#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace oxbow {

/// One received video frame, always BGRA. `bottomUp` says whether rows are
/// already in OpenGL order (only NDI on Windows delivers that); the pump
/// normalises at ingest.
struct VideoFrame {
  int width = 0;
  int height = 0;
  int strideBytes = 0;
  const uint8_t* data = nullptr;
  bool bottomUp = false;
  int frameRateN = 60000;
  int frameRateD = 1000;
  int64_t timestamp = 0;// 100 ns units — NDI and OMT agree on this.
};

/// Passthrough audio, normalised to planar float32 — the native layout of
/// both NDI (FLTP) and OMT (FPA1), so bridging protocols is a straight copy.
struct AudioFrame {
  int sampleRate = 48000;
  int channels = 0;
  int samplesPerChannel = 0;
  std::vector<float> data;// channels planes of samplesPerChannel floats.
  int64_t timestamp = 0;
};

struct SourceInfo {
  std::string name;
  std::string url;
};

class VideoReceiver {
 public:
  virtual ~VideoReceiver() = default;

  enum class Captured { none, video, audio };

  virtual const std::string& sourceName() const = 0;

  /// Waits up to `timeoutMs`. On video, `onVideo` runs with a frame whose data
  /// is only valid during the callback. On audio, take it with takeAudio().
  virtual Captured capture(
      unsigned timeoutMs,
      const std::function<void(const VideoFrame&)>& onVideo) = 0;

  virtual std::unique_ptr<AudioFrame> takeAudio() = 0;
};

class VideoSender {
 public:
  virtual ~VideoSender() = default;

  /// `data` is BGRA, top-down, tightly packed. `timestamp` in 100 ns units,
  /// or -1 to let the transport synthesise one.
  virtual void sendVideo(const uint8_t* data, int width, int height,
                         int frameRateN, int frameRateD,
                         int64_t timestamp) = 0;
  virtual void sendAudio(const AudioFrame& frame) = 0;
};

/// Factories keyed on protocol name, "ndi" or "omt".
std::vector<SourceInfo> listSources(const std::string& protocol,
                                    unsigned waitMs, std::string& error);
std::unique_ptr<VideoReceiver> connectReceiver(const std::string& protocol,
                                               const std::string& nameSubstring,
                                               unsigned waitMs,
                                               std::string& error);
std::unique_ptr<VideoSender> createSender(const std::string& protocol,
                                          const std::string& name,
                                          std::string& error);

}  // namespace oxbow
