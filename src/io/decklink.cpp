// DeckLink output.
//
// Scheduled playback: the card runs a timeline, the host keeps a few frames
// ahead of it, and the card displays each one at the moment it was scheduled
// for. That is the arrangement Blackmagic recommends and the only one that
// gives smooth output — `DisplayVideoFrameSync` is simpler and judders.
//
// Ported from WebLinked's `src/outputs/decklink_output.cpp`, which has been run
// on a real Duo 2. What changed on the way across:
//
//   - **BGRA on the wire, not UYVY.** oxbow's pump already holds BGRA and the
//     card accepts `bmdFormat8BitBGRA`, so there is no colour conversion in
//     this file at all. WebLinked converts because CEF hands it BGRA and its
//     SDI path was built around 4:2:2; here the straight path is available and
//     a conversion would only be a chance to get BT.709 wrong.
//   - **The raster comes from the first frame**, not from a format passed in,
//     because that is how oxbow's senders learn their size.
//   - **No keying and no audio.** See decklink.h.
//
// **Never run against hardware by its author.** The SDK sequence follows
// WebLinked's, which was proven on a Duo 2, but no DeckLink has been connected
// to *this* code. Do not describe it as working until one has been.
//
// The SDK is not vendored — it is Blackmagic's, and its licence is not ours to
// redistribute. Configure with `-DDECKLINK_SDK_DIR=/path/to/SDK` and the CMake
// picks up the headers and `DeckLinkAPIDispatch.cpp`. Note there are commonly
// several SDK copies on a developer's machine at different versions; anything
// below 11.0 lacks `IDeckLinkProfileAttributes` and will not compile.

#include "io/decklink.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "DeckLinkAPI.h"

namespace oxbow {
namespace {

/// The card's timeline unit. Taken from the display mode itself rather than
/// computed, so an odd rate like 59.94 stays exact instead of being rounded
/// into a slow drift.
constexpr BMDTimeScale kUnsetScale = 0;

/// How many frames to keep ahead of the card. Three is Blackmagic's usual
/// suggestion: enough that a late tick does not underflow, few enough that the
/// added latency stays around one frame at 50/60p.
constexpr int kPreRollFrames = 3;

#if defined(__APPLE__)
/// macOS returns CFStringRef from GetDisplayName; Windows returns BSTR and
/// Linux a plain char*. One place knows which.
std::string toStdString(CFStringRef value) {
  if (value == nullptr) return {};
  char buffer[256] = {};
  const bool ok = CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8);
  CFRelease(value);
  return ok ? std::string(buffer) : std::string();
}
using DeckLinkString = CFStringRef;
#elif defined(_WIN32)
std::string toStdString(BSTR value) {
  if (value == nullptr) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  std::string out(length > 0 ? length - 1 : 0, '\0');
  if (length > 0) {
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), length, nullptr, nullptr);
  }
  SysFreeString(value);
  return out;
}
using DeckLinkString = BSTR;
#else
std::string toStdString(const char* value) {
  if (value == nullptr) return {};
  std::string out(value);
  free(const_cast<char*>(value));
  return out;
}
using DeckLinkString = const char*;
#endif

/// Releases our reference to a frame once the card has finished displaying it.
///
/// The card holds its own reference from ScheduleVideoFrame, so a frame stays
/// alive until *both* are gone. Releasing at schedule time instead would free
/// the buffer while the card is still reading it, which shows up as tearing or
/// as nothing at all, depending on the allocator's mood.
class CompletionCallback final : public IDeckLinkVideoOutputCallback {
 public:
  HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(
      IDeckLinkVideoFrame* completedFrame,
      BMDOutputFrameCompletionResult result) override {
    if (result == bmdOutputFrameDisplayedLate) ++late_;
    if (result == bmdOutputFrameDropped) ++dropped_;
    if (completedFrame != nullptr) completedFrame->Release();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override { return S_OK; }

  // The card never queries this object across apartments, and its lifetime is
  // the sender's, so the reference count is a formality the interface requires.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  int64_t late() const { return late_.load(std::memory_order_relaxed); }
  int64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int64_t> late_{0};
  std::atomic<int64_t> dropped_{0};
};

class DeckLinkSender final : public VideoSender {
 public:
  explicit DeckLinkSender(std::string selector) : selector_(std::move(selector)) {}

  ~DeckLinkSender() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (output_ != nullptr) {
      if (playing_) {
        BMDTimeValue stoppedAt = 0;
        output_->StopScheduledPlayback(0, &stoppedAt, timeScale_);
      }
      output_->SetScheduledFrameCompletionCallback(nullptr);
      output_->DisableVideoOutput();
      output_->Release();
    }
    if (device_ != nullptr) device_->Release();

    if (scheduled_ > 0) {
      std::fprintf(stderr, "decklink: %lld frames, %lld late, %lld dropped\n",
                   (long long)scheduled_, (long long)callback_.late(),
                   (long long)callback_.dropped());
    }
  }

  void sendVideo(const uint8_t* data, int width, int height, int frameRateN,
                 int frameRateD, int64_t /*timestamp*/) override {
    if (data == nullptr || width <= 0 || height <= 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureOpen(width, height, frameRateN, frameRateD)) return;

    IDeckLinkMutableVideoFrame* frame = nullptr;
    if (output_->CreateVideoFrame(width, height, width * 4, bmdFormat8BitBGRA,
                                  bmdFrameFlagDefault, &frame) != S_OK ||
        frame == nullptr) {
      return;
    }

    void* bytes = nullptr;
    if (frame->GetBytes(&bytes) == S_OK && bytes != nullptr) {
      // The card chooses its own row alignment, which need not be width*4.
      const long destinationStride = frame->GetRowBytes();
      const size_t sourceStride = static_cast<size_t>(width) * 4;
      auto* destination = static_cast<uint8_t*>(bytes);
      if (static_cast<size_t>(destinationStride) == sourceStride) {
        std::memcpy(destination, data, sourceStride * static_cast<size_t>(height));
      } else {
        for (int y = 0; y < height; ++y) {
          std::memcpy(destination + static_cast<size_t>(y) * destinationStride,
                      data + static_cast<size_t>(y) * sourceStride, sourceStride);
        }
      }
    }

    scheduleFrame(frame);
  }

  /// See decklink.h: audio is deliberately not put on the card.
  void sendAudio(const AudioFrame&) override {}

 private:
  /// Schedules at the next free slot on the card's timeline.
  ///
  /// Display times come from a monotonically increasing frame index rather than
  /// from wall-clock arithmetic, so a late tick shortens the queue instead of
  /// scheduling a frame in the past — which the card rejects outright.
  void scheduleFrame(IDeckLinkMutableVideoFrame* frame) {
    if (output_->ScheduleVideoFrame(frame, scheduled_ * frameDuration_, frameDuration_,
                                    timeScale_) != S_OK) {
      frame->Release();
      return;
    }
    ++scheduled_;

    // Playback starts once there is a queue to play, not on the first frame:
    // starting with one frame in hand underflows immediately.
    if (!playing_ && scheduled_ >= kPreRollFrames) {
      if (output_->StartScheduledPlayback(0, timeScale_, 1.0) == S_OK) {
        playing_ = true;
      }
    }
  }

  bool ensureOpen(int width, int height, int frameRateN, int frameRateD) {
    if (open_) {
      // A raster change mid-run would need the whole output torn down and
      // re-enabled. Refuse instead of half-doing it, and say so once.
      if (width != width_ || height != height_) {
        if (!warnedRaster_) {
          warnedRaster_ = true;
          std::fprintf(stderr,
                       "decklink: raster changed to %dx%d; this output stays at %dx%d\n",
                       width, height, width_, height_);
        }
        return false;
      }
      return true;
    }
    if (failed_) return false;

    if (!openDevice()) return fail();
    if (!findDisplayMode(width, height, frameRateN, frameRateD)) return fail();

    if (output_->SetScheduledFrameCompletionCallback(&callback_) != S_OK) {
      std::fprintf(stderr, "decklink: SetScheduledFrameCompletionCallback failed\n");
      return fail();
    }
    if (output_->EnableVideoOutput(displayMode_, bmdVideoOutputFlagDefault) != S_OK) {
      std::fprintf(stderr, "decklink: EnableVideoOutput failed on \"%s\"\n",
                   deviceName_.c_str());
      return fail();
    }

    width_ = width;
    height_ = height;
    open_ = true;
    std::fprintf(stderr, "decklink: \"%s\" %dx%d %.3f fps, BGRA\n", deviceName_.c_str(),
                 width, height,
                 frameDuration_ > 0 ? double(timeScale_) / double(frameDuration_) : 0.0);
    return true;
  }

  bool fail() {
    failed_ = true;
    return false;
  }

  bool openDevice() {
    IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
    if (iterator == nullptr) {
      std::fprintf(stderr,
                   "decklink: no DeckLink drivers found — is Desktop Video installed?\n");
      return false;
    }

    // A decimal selector is an index; anything else is matched against the
    // device's own name. Empty takes the first, which is what a machine with
    // one card wants.
    const bool byIndex = !selector_.empty() &&
                         selector_.find_first_not_of("0123456789") == std::string::npos;
    const int wanted = byIndex ? std::atoi(selector_.c_str()) : -1;

    std::vector<std::string> seen;
    IDeckLink* device = nullptr;
    int index = 0;
    while (iterator->Next(&device) == S_OK) {
      DeckLinkString rawName = nullptr;
      device->GetDisplayName(&rawName);
      const std::string name = toStdString(rawName);
      seen.push_back(name);

      const bool matches = selector_.empty()
                               ? index == 0
                               : (byIndex ? index == wanted
                                          : name.find(selector_) != std::string::npos);
      if (matches && device->QueryInterface(IID_IDeckLinkOutput, (void**)&output_) == S_OK) {
        device_ = device;
        deviceName_ = name;
        break;
      }
      device->Release();
      device = nullptr;
      ++index;
    }
    iterator->Release();

    if (output_ == nullptr) {
      std::string list;
      for (size_t i = 0; i < seen.size(); ++i) {
        list += (i ? ", " : "") + std::to_string(i) + ": " + seen[i];
      }
      std::fprintf(stderr, "decklink: no output device matching \"%s\" (%s)\n",
                   selector_.c_str(),
                   list.empty() ? "no devices found" : list.c_str());
      return false;
    }
    return true;
  }

  bool findDisplayMode(int width, int height, int frameRateN, int frameRateD) {
    IDeckLinkDisplayModeIterator* iterator = nullptr;
    if (output_->GetDisplayModeIterator(&iterator) != S_OK || iterator == nullptr) {
      std::fprintf(stderr, "decklink: GetDisplayModeIterator failed\n");
      return false;
    }

    std::string offered;
    IDeckLinkDisplayMode* mode = nullptr;
    bool found = false;
    while (iterator->Next(&mode) == S_OK) {
      BMDTimeValue duration = 0;
      BMDTimeScale scale = kUnsetScale;
      mode->GetFrameRate(&duration, &scale);

      const bool sizeMatches = mode->GetWidth() == width && mode->GetHeight() == height;
      // Cross-multiplied rationals: duration/scale is the period, so the rate
      // is scale/duration. Never compare these as doubles — 59.94 is 60000/1001
      // and nothing else.
      const bool rateMatches = duration > 0 && frameRateD > 0 &&
                               static_cast<int64_t>(scale) * frameRateD ==
                                   static_cast<int64_t>(duration) * frameRateN;

      if (sizeMatches && rateMatches) {
        displayMode_ = mode->GetDisplayMode();
        frameDuration_ = duration;
        timeScale_ = scale;
        found = true;
        mode->Release();
        break;
      }
      if (sizeMatches) {
        offered += (offered.empty() ? "" : ", ") + std::to_string(scale) + "/" +
                   std::to_string(duration);
      }
      mode->Release();
    }
    iterator->Release();

    if (!found) {
      std::fprintf(stderr, "decklink: \"%s\" cannot do %dx%d @ %d/%d%s%s\n",
                   deviceName_.c_str(), width, height, frameRateN, frameRateD,
                   offered.empty() ? "" : " — it offers these rates at that raster: ",
                   offered.c_str());
      return false;
    }

    // Ask the card as well: enumeration lists what the hardware knows, not what
    // this connection and pixel format can actually carry.
    BMDDisplayMode actualMode = displayMode_;
    bool supported = false;
    if (output_->DoesSupportVideoMode(bmdVideoConnectionUnspecified, displayMode_,
                                      bmdFormat8BitBGRA, bmdNoVideoOutputConversion,
                                      bmdSupportedVideoModeDefault, &actualMode,
                                      &supported) == S_OK &&
        !supported) {
      std::fprintf(stderr, "decklink: \"%s\" will not carry that mode as 8-bit BGRA\n",
                   deviceName_.c_str());
      return false;
    }
    return true;
  }

  std::string selector_;
  std::string deviceName_;
  std::mutex mutex_;

  IDeckLink* device_ = nullptr;
  IDeckLinkOutput* output_ = nullptr;
  CompletionCallback callback_;

  BMDDisplayMode displayMode_ = bmdModeHD1080p50;
  BMDTimeValue frameDuration_ = 0;
  BMDTimeScale timeScale_ = kUnsetScale;

  int width_ = 0;
  int height_ = 0;
  int64_t scheduled_ = 0;
  bool open_ = false;
  bool playing_ = false;
  bool failed_ = false;
  bool warnedRaster_ = false;
};

}  // namespace

std::unique_ptr<VideoSender> decklinkCreateSender(const std::string& name,
                                                  std::string& error) {
  (void)error;
  // Device selection is deferred with everything else to the first frame, so a
  // missing card is reported by the backend rather than guessed at here.
  return std::make_unique<DeckLinkSender>(name);
}

}  // namespace oxbow
