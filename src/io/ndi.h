#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace oxbow {

/// One received video frame, always BGRA. `bottomUp` says whether rows are
/// already in OpenGL order: true on Windows (the receiver asks NDI for the
/// _flipped variant, which only exists there), false elsewhere, where the
/// pump does one CPU row flip at ingest.
struct VideoFrame {
  int width = 0;
  int height = 0;
  int strideBytes = 0;
  const uint8_t* data = nullptr;
  bool bottomUp = false;
  int frameRateN = 60000;
  int frameRateD = 1000;
  int64_t timestamp = 0;
};

/// Opaque passthrough audio frame: captured from the receiver and handed to
/// the sender untouched. The concrete type lives in ndi.cpp; this base only
/// exists so a unique_ptr can carry it across the pump.
struct AudioFrame {
  virtual ~AudioFrame() = default;
};

struct NdiSource {
  std::string name;// Full NDI name, "HOST (source)".
  std::string url;
};

/// Discovers sources currently visible on the network.
std::vector<NdiSource> ndiListSources(unsigned waitMs, std::string& error);

class NdiReceiver {
 public:
  virtual ~NdiReceiver() = default;

  /// Connects to the first source whose full name contains `nameSubstring`
  /// (case-insensitive). Waits up to `waitMs` for discovery.
  static std::unique_ptr<NdiReceiver> connect(const std::string& nameSubstring,
                                              unsigned waitMs,
                                              std::string& error);

  /// Name actually connected to.
  virtual const std::string& sourceName() const = 0;

  enum class Captured { none, video, audio };

  /// Waits up to `timeoutMs` for a frame. On `video`, `onVideo` is called with
  /// a frame whose data is only valid during the callback. On `audio`, the
  /// frame is stored and can be taken with takeAudio().
  virtual Captured capture(unsigned timeoutMs,
                           const std::function<void(const VideoFrame&)>& onVideo) = 0;

  virtual std::unique_ptr<AudioFrame> takeAudio() = 0;
};

class NdiSender {
 public:
  virtual ~NdiSender() = default;

  static std::unique_ptr<NdiSender> create(const std::string& name,
                                           std::string& error);

  /// `data` is BGRA, top-down, tightly packed.
  virtual void sendVideo(const uint8_t* data, int width, int height,
                         int frameRateN, int frameRateD) = 0;
  virtual void sendAudio(const AudioFrame& frame) = 0;
};

}  // namespace oxbow
