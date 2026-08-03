// Open Media Transport receive and send.
//
// OMT is the open, MIT-licensed alternative to NDI, from the vMix people —
// and the licence-clean route into vMix 29, which speaks it natively. libomt
// is a C wrapper around the .NET libomtnet; published binaries cover Windows
// x64/arm64 and macOS (universal), with libvmx (the VMX codec) and
// libomtnet.dll expected beside libomt. There is no Linux binary. All of that
// is why this backend is loaded at run time like NDI: on a machine without
// the library the protocol reports itself unavailable and the rest works.
//
// Video is requested as BGRA. OMT has no bottom-up delivery option, so frames
// are top-down and the pump's ingest flip applies everywhere. Audio is FPA1 —
// planar float32, contiguous planes — which is exactly the shared AudioFrame
// layout, so conversion is one memcpy.

#include "io/omt.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <cstdio>

#include <libomt.h>

#include "core/dylib.h"

namespace oxbow {
namespace {

#if defined(__APPLE__)
#define OMT_LIBRARY_NAME "libomt.dylib"
#elif defined(_WIN32)
#define OMT_LIBRARY_NAME "libomt.dll"
#else
#define OMT_LIBRARY_NAME "libomt.so"
#endif

// Receivers may hand us BGRX for alpha-less senders; the enum has no name for
// it, but the FourCC is documented.
constexpr uint32_t kFourccBgrx = 0x58524742;

struct OmtApi {
  void (*setloggingcallback)(OMTLoggingCallback) = nullptr;
  char** (*discovery_getaddresses)(int*) = nullptr;

  omt_receive_t* (*receive_create)(const char*, OMTFrameType,
                                   OMTPreferredVideoFormat,
                                   OMTReceiveFlags) = nullptr;
  void (*receive_destroy)(omt_receive_t*) = nullptr;
  OMTMediaFrame* (*receive)(omt_receive_t*, OMTFrameType, int) = nullptr;

  omt_send_t* (*send_create)(const char*, OMTQuality) = nullptr;
  void (*send_destroy)(omt_send_t*) = nullptr;
  int (*send)(omt_send_t*, OMTMediaFrame*) = nullptr;
};

class OmtRuntime {
 public:
  static const OmtApi* acquire(std::string& error) {
    static OmtRuntime runtime;
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    if (!runtime.loaded_) {
      runtime.load();
      runtime.loaded_ = true;
    }
    if (!runtime.ok_) {
      error = runtime.error_;
      return nullptr;
    }
    return &runtime.api_;
  }

 private:
  void load() {
    std::vector<std::string> candidates = {OMT_LIBRARY_NAME};
#if defined(__APPLE__)
    candidates.push_back("/usr/local/lib/libomt.dylib");
#endif
    if (!dylib_.open(candidates)) {
      error_ = "OMT runtime not found (" + dylib_.lastError() +
               ") — get libomt from "
               "github.com/openmediatransport/libomtnet/releases and put it "
               "on the library path (libvmx and libomtnet.dll go beside it)";
      return;
    }
    struct Entry {
      const char* name;
      void** slot;
    };
    const Entry entries[] = {
        {"omt_discovery_getaddresses", (void**)&api_.discovery_getaddresses},
        {"omt_receive_create", (void**)&api_.receive_create},
        {"omt_receive_destroy", (void**)&api_.receive_destroy},
        {"omt_receive", (void**)&api_.receive},
        {"omt_send_create", (void**)&api_.send_create},
        {"omt_send_destroy", (void**)&api_.send_destroy},
        {"omt_send", (void**)&api_.send},
    };
    for (const Entry& entry : entries) {
      *entry.slot = dylib_.rawSymbol(entry.name);
      if (!*entry.slot) {
        error_ = std::string("OMT runtime is missing ") + entry.name;
        return;
      }
    }
    // Optional; present in the published builds. libomt's internals fail
    // quietly (a send that cannot encode just does nothing), so this is the
    // only window into what it is doing — but its discovery chatter is heavy,
    // so it stays off unless asked for.
    if (std::getenv("OXBOW_OMT_LOG")) {
      if (void* setLog = dylib_.rawSymbol("omt_setloggingcallback")) {
        reinterpret_cast<void (*)(OMTLoggingCallback)>(setLog)(
            [](const char* message) {
              std::fprintf(stderr, "[omt] %s\n", message ? message : "");
            });
      }
    }
    ok_ = true;
  }

  std::mutex mutex_;
  Dylib dylib_;
  OmtApi api_;
  bool loaded_ = false;
  bool ok_ = false;
  std::string error_;
};

bool nameContains(const std::string& haystack, const std::string& needle) {
  auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
  };
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

class OmtReceiverImpl final : public VideoReceiver {
 public:
  ~OmtReceiverImpl() override {
    if (recv_) api_->receive_destroy(recv_);
  }

  bool init(const std::string& nameSubstring, unsigned waitMs,
            std::string& error) {
    api_ = OmtRuntime::acquire(error);
    if (!api_) return false;

    // An omt:// URL connects directly; anything else goes through discovery.
    if (nameSubstring.rfind("omt://", 0) == 0) {
      sourceName_ = nameSubstring;
    } else {
      std::vector<SourceInfo> sources = omtListSources(waitMs, error);
      for (const SourceInfo& source : sources) {
        if (nameContains(source.name, nameSubstring)) {
          sourceName_ = source.name;
          break;
        }
      }
      if (sourceName_.empty()) {
        error = "no OMT source matching \"" + nameSubstring + "\" (saw " +
                std::to_string(sources.size()) + " sources)";
        return false;
      }
    }

    recv_ = api_->receive_create(
        sourceName_.c_str(),
        (OMTFrameType)(OMTFrameType_Video | OMTFrameType_Audio),
        OMTPreferredVideoFormat_BGRA, OMTReceiveFlags_None);
    if (!recv_) {
      error = "omt_receive_create failed for " + sourceName_;
      return false;
    }
    return true;
  }

  const std::string& sourceName() const override { return sourceName_; }

  Captured capture(
      unsigned timeoutMs,
      const std::function<void(const VideoFrame&)>& onVideo) override {
    OMTMediaFrame* media = api_->receive(
        recv_, (OMTFrameType)(OMTFrameType_Video | OMTFrameType_Audio),
        (int)timeoutMs);
    if (!media) return Captured::none;

    if (media->Type == OMTFrameType_Video) {
      if (media->Codec != OMTCodec_BGRA &&
          (uint32_t)media->Codec != kFourccBgrx)
        return Captured::none;// UYVY fallback not handled yet.
      VideoFrame frame;
      frame.width = media->Width;
      frame.height = media->Height;
      frame.strideBytes = media->Stride;
      frame.data = static_cast<const uint8_t*>(media->Data);
      frame.bottomUp = false;
      frame.frameRateN = media->FrameRateN;
      frame.frameRateD = media->FrameRateD;
      frame.timestamp = media->Timestamp;
      if (onVideo) onVideo(frame);
      return Captured::video;
    }

    if (media->Type == OMTFrameType_Audio) {
      auto held = std::make_unique<AudioFrame>();
      held->sampleRate = media->SampleRate;
      held->channels = media->Channels;
      held->samplesPerChannel = media->SamplesPerChannel;
      held->timestamp = media->Timestamp;
      const size_t floats = (size_t)media->Channels * media->SamplesPerChannel;
      held->data.resize(floats);
      std::memcpy(held->data.data(), media->Data, floats * sizeof(float));
      audio_ = std::move(held);
      return Captured::audio;
    }
    return Captured::none;
  }

  std::unique_ptr<AudioFrame> takeAudio() override { return std::move(audio_); }

 private:
  const OmtApi* api_ = nullptr;
  omt_receive_t* recv_ = nullptr;
  std::string sourceName_;
  std::unique_ptr<AudioFrame> audio_;
};

class OmtSenderImpl final : public VideoSender {
 public:
  ~OmtSenderImpl() override {
    if (send_) api_->send_destroy(send_);
  }

  bool init(const std::string& name, std::string& error) {
    api_ = OmtRuntime::acquire(error);
    if (!api_) return false;
    send_ = api_->send_create(name.c_str(), OMTQuality_Default);
    if (!send_) {
      error = "omt_send_create failed for " + name;
      return false;
    }
    return true;
  }

  void sendVideo(const uint8_t* data, int width, int height, int frameRateN,
                 int frameRateD, int64_t timestamp) override {
    OMTMediaFrame frame = {};
    frame.Type = OMTFrameType_Video;
    frame.Codec = OMTCodec_BGRA;
    frame.Width = width;
    frame.Height = height;
    frame.Stride = width * 4;
    frame.Flags = OMTVideoFlags_None;
    frame.FrameRateN = frameRateN;
    frame.FrameRateD = frameRateD;
    frame.AspectRatio = (float)width / (float)height;
    frame.ColorSpace = height >= 720 ? OMTColorSpace_BT709
                                     : OMTColorSpace_BT601;
    frame.Timestamp = timestamp;
    frame.Data = const_cast<uint8_t*>(data);
    frame.DataLength = width * 4 * height;
    api_->send(send_, &frame);
  }

  void sendAudio(const AudioFrame& audio) override {
    OMTMediaFrame frame = {};
    frame.Type = OMTFrameType_Audio;
    frame.Codec = OMTCodec_FPA1;
    frame.SampleRate = audio.sampleRate;
    frame.Channels = audio.channels;
    frame.SamplesPerChannel = audio.samplesPerChannel;
    frame.Timestamp = audio.timestamp;
    frame.Data = const_cast<float*>(audio.data.data());
    frame.DataLength = (int)(audio.data.size() * sizeof(float));
    api_->send(send_, &frame);
  }

 private:
  const OmtApi* api_ = nullptr;
  omt_send_t* send_ = nullptr;
};

}  // namespace

std::vector<SourceInfo> omtListSources(unsigned waitMs, std::string& error) {
  const OmtApi* api = OmtRuntime::acquire(error);
  if (!api) return {};

  // getaddresses returns whatever discovery has heard so far, so poll until
  // something shows up or the wait expires — mirroring NDI's wait_for_sources.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
  std::vector<SourceInfo> result;
  for (;;) {
    int count = 0;
    char** addresses = api->discovery_getaddresses(&count);
    result.clear();
    for (int i = 0; i < count; ++i) {
      if (!addresses[i]) continue;
      SourceInfo source;
      source.name = addresses[i];
      result.push_back(std::move(source));
    }
    if (!result.empty() || std::chrono::steady_clock::now() >= deadline)
      return result;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

std::unique_ptr<VideoReceiver> omtConnectReceiver(
    const std::string& nameSubstring, unsigned waitMs, std::string& error) {
  auto receiver = std::make_unique<OmtReceiverImpl>();
  if (!receiver->init(nameSubstring, waitMs, error)) return nullptr;
  return receiver;
}

std::unique_ptr<VideoSender> omtCreateSender(const std::string& name,
                                             std::string& error) {
  auto sender = std::make_unique<OmtSenderImpl>();
  if (!sender->init(name, error)) return nullptr;
  return sender;
}

}  // namespace oxbow
