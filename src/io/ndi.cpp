// NDI receive and send.
//
// libndi is opened with dlopen and its flat C entry points resolved by name,
// for the same two reasons as WebLinked (whose sender this borrows from): the
// NDI licence does not allow an MIT repository to carry the binary, and the
// flat symbols work across NDI 5 and 6 runtimes where the versioned
// NDIlib_v6_load() struct would not.
//
// The receiver asks for BGRX_BGRA_flipped so video arrives bottom-up — which
// is OpenGL's row order, so frames upload into a texture with no CPU flip.
// The send side has no flipped variant, so the pump flips rows once on the
// way out.

#include "io/ndi.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

#define NDILIB_CPP_DEFAULT_CONSTRUCTORS 0
#include <Processing.NDI.Lib.h>

#include "core/dylib.h"

namespace oxbow {
namespace {

#if defined(__APPLE__)
#define NDI_LIBRARY_NAME "libndi.dylib"
#elif defined(_WIN32)
#define NDI_LIBRARY_NAME "Processing.NDI.Lib.x64.dll"
#else
#define NDI_LIBRARY_NAME "libndi.so.6"
#endif

struct NdiApi {
  bool (*initialize)() = nullptr;
  const char* (*version)() = nullptr;

  NDIlib_find_instance_t (*find_create_v2)(const NDIlib_find_create_t*) = nullptr;
  void (*find_destroy)(NDIlib_find_instance_t) = nullptr;
  bool (*find_wait_for_sources)(NDIlib_find_instance_t, uint32_t) = nullptr;
  const NDIlib_source_t* (*find_get_current_sources)(NDIlib_find_instance_t,
                                                     uint32_t*) = nullptr;

  NDIlib_recv_instance_t (*recv_create_v3)(const NDIlib_recv_create_v3_t*) = nullptr;
  void (*recv_destroy)(NDIlib_recv_instance_t) = nullptr;
  void (*recv_connect)(NDIlib_recv_instance_t, const NDIlib_source_t*) = nullptr;
  NDIlib_frame_type_e (*recv_capture_v3)(NDIlib_recv_instance_t,
                                         NDIlib_video_frame_v2_t*,
                                         NDIlib_audio_frame_v3_t*,
                                         NDIlib_metadata_frame_t*,
                                         uint32_t) = nullptr;
  void (*recv_free_video_v2)(NDIlib_recv_instance_t,
                             const NDIlib_video_frame_v2_t*) = nullptr;
  void (*recv_free_audio_v3)(NDIlib_recv_instance_t,
                             const NDIlib_audio_frame_v3_t*) = nullptr;
  void (*recv_free_metadata)(NDIlib_recv_instance_t,
                             const NDIlib_metadata_frame_t*) = nullptr;

  NDIlib_send_instance_t (*send_create)(const NDIlib_send_create_t*) = nullptr;
  void (*send_destroy)(NDIlib_send_instance_t) = nullptr;
  void (*send_send_video_v2)(NDIlib_send_instance_t,
                             const NDIlib_video_frame_v2_t*) = nullptr;
  void (*send_send_audio_v3)(NDIlib_send_instance_t,
                             const NDIlib_audio_frame_v3_t*) = nullptr;
};

class NdiRuntime {
 public:
  static const NdiApi* acquire(std::string& error) {
    static NdiRuntime runtime;
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
    std::vector<std::string> candidates = {NDI_LIBRARY_NAME};
#if defined(__APPLE__)
    candidates.push_back("/usr/local/lib/libndi.dylib");
#endif
    if (!dylib_.open(candidates)) {
      error_ = "NDI runtime not found (" + dylib_.lastError() +
               ") — install it from " NDILIB_REDIST_URL;
      return;
    }
    struct Entry {
      const char* name;
      void** slot;
    };
    const Entry entries[] = {
        {"NDIlib_initialize", (void**)&api_.initialize},
        {"NDIlib_version", (void**)&api_.version},
        {"NDIlib_find_create_v2", (void**)&api_.find_create_v2},
        {"NDIlib_find_destroy", (void**)&api_.find_destroy},
        {"NDIlib_find_wait_for_sources", (void**)&api_.find_wait_for_sources},
        {"NDIlib_find_get_current_sources",
         (void**)&api_.find_get_current_sources},
        {"NDIlib_recv_create_v3", (void**)&api_.recv_create_v3},
        {"NDIlib_recv_destroy", (void**)&api_.recv_destroy},
        {"NDIlib_recv_connect", (void**)&api_.recv_connect},
        {"NDIlib_recv_capture_v3", (void**)&api_.recv_capture_v3},
        {"NDIlib_recv_free_video_v2", (void**)&api_.recv_free_video_v2},
        {"NDIlib_recv_free_audio_v3", (void**)&api_.recv_free_audio_v3},
        {"NDIlib_recv_free_metadata", (void**)&api_.recv_free_metadata},
        {"NDIlib_send_create", (void**)&api_.send_create},
        {"NDIlib_send_destroy", (void**)&api_.send_destroy},
        {"NDIlib_send_send_video_v2", (void**)&api_.send_send_video_v2},
        {"NDIlib_send_send_audio_v3", (void**)&api_.send_send_audio_v3},
    };
    for (const Entry& entry : entries) {
      *entry.slot = dylib_.rawSymbol(entry.name);
      if (!*entry.slot) {
        error_ = std::string("NDI runtime is missing ") + entry.name;
        return;
      }
    }
    if (!api_.initialize()) {
      error_ = "NDIlib_initialize failed (unsupported CPU?)";
      return;
    }
    ok_ = true;
  }

  std::mutex mutex_;
  Dylib dylib_;
  NdiApi api_;
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

}  // namespace

namespace {
struct AudioFrameImpl final : AudioFrame {
  NDIlib_audio_frame_v3_t frame = {};
  std::vector<uint8_t> data;
};
}  // namespace

std::vector<NdiSource> ndiListSources(unsigned waitMs, std::string& error) {
  const NdiApi* api = NdiRuntime::acquire(error);
  if (!api) return {};

  NDIlib_find_create_t createSettings = {};
  createSettings.show_local_sources = true;
  NDIlib_find_instance_t finder = api->find_create_v2(&createSettings);
  if (!finder) {
    error = "NDIlib_find_create_v2 failed";
    return {};
  }
  api->find_wait_for_sources(finder, waitMs);
  uint32_t count = 0;
  const NDIlib_source_t* sources = api->find_get_current_sources(finder, &count);
  std::vector<NdiSource> result;
  for (uint32_t i = 0; i < count; ++i) {
    NdiSource source;
    if (sources[i].p_ndi_name) source.name = sources[i].p_ndi_name;
    if (sources[i].p_url_address) source.url = sources[i].p_url_address;
    result.push_back(std::move(source));
  }
  api->find_destroy(finder);
  return result;
}

namespace {

class NdiReceiverImpl final : public NdiReceiver {
 public:
  ~NdiReceiverImpl() override {
    if (recv_) api_->recv_destroy(recv_);
  }

  bool init(const std::string& nameSubstring, unsigned waitMs,
            std::string& error) {
    api_ = NdiRuntime::acquire(error);
    if (!api_) return false;

    std::vector<NdiSource> sources = ndiListSources(waitMs, error);
    const NdiSource* match = nullptr;
    for (const NdiSource& source : sources) {
      if (nameContains(source.name, nameSubstring)) {
        match = &source;
        break;
      }
    }
    if (!match) {
      error = "no NDI source matching \"" + nameSubstring + "\" (saw " +
              std::to_string(sources.size()) + " sources)";
      return false;
    }
    sourceName_ = match->name;

    NDIlib_recv_create_v3_t settings = {};
    NDIlib_source_t source = {};
    source.p_ndi_name = sourceName_.c_str();
    source.p_url_address = match->url.empty() ? nullptr : match->url.c_str();
    settings.source_to_connect_to = source;
#if defined(_WIN32)
    settings.color_format = (NDIlib_recv_color_format_e)
        NDIlib_recv_color_format_BGRX_BGRA_flipped;
#else
    settings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
#endif
    settings.bandwidth = NDIlib_recv_bandwidth_highest;
    settings.allow_video_fields = false;
    settings.p_ndi_recv_name = "oxbow";
    recv_ = api_->recv_create_v3(&settings);
    if (!recv_) {
      error = "NDIlib_recv_create_v3 failed";
      return false;
    }
    return true;
  }

  const std::string& sourceName() const override { return sourceName_; }

  Captured capture(
      unsigned timeoutMs,
      const std::function<void(const VideoFrame&)>& onVideo) override {
    NDIlib_video_frame_v2_t video = {};
    NDIlib_audio_frame_v3_t audio = {};
    NDIlib_metadata_frame_t metadata = {};
    switch (api_->recv_capture_v3(recv_, &video, &audio, &metadata,
                                  timeoutMs)) {
      case NDIlib_frame_type_video: {
        VideoFrame frame;
        frame.width = video.xres;
        frame.height = video.yres;
        frame.strideBytes = video.line_stride_in_bytes;
        frame.data = video.p_data;
#if defined(_WIN32)
        frame.bottomUp = true;
#endif
        frame.frameRateN = video.frame_rate_N;
        frame.frameRateD = video.frame_rate_D;
        frame.timestamp = video.timestamp;
        if (onVideo) onVideo(frame);
        api_->recv_free_video_v2(recv_, &video);
        return Captured::video;
      }
      case NDIlib_frame_type_audio: {
        auto held = std::make_unique<AudioFrameImpl>();
        held->frame = audio;
        const size_t bytes =
            (size_t)audio.channel_stride_in_bytes * audio.no_channels;
        held->data.assign(audio.p_data, audio.p_data + bytes);
        held->frame.p_data = held->data.data();
        held->frame.p_metadata = nullptr;
        api_->recv_free_audio_v3(recv_, &audio);
        audio_ = std::move(held);
        return Captured::audio;
      }
      case NDIlib_frame_type_metadata:
        api_->recv_free_metadata(recv_, &metadata);
        return Captured::none;
      default:
        return Captured::none;
    }
  }

  std::unique_ptr<AudioFrame> takeAudio() override { return std::move(audio_); }

 private:
  const NdiApi* api_ = nullptr;
  NDIlib_recv_instance_t recv_ = nullptr;
  std::string sourceName_;
  std::unique_ptr<AudioFrameImpl> audio_;
};

class NdiSenderImpl final : public NdiSender {
 public:
  ~NdiSenderImpl() override {
    if (send_) api_->send_destroy(send_);
  }

  bool init(const std::string& name, std::string& error) {
    api_ = NdiRuntime::acquire(error);
    if (!api_) return false;
    name_ = name;
    NDIlib_send_create_t settings = {};
    settings.p_ndi_name = name_.c_str();
    settings.clock_video = false;// The receive side paces the loop.
    settings.clock_audio = false;
    send_ = api_->send_create(&settings);
    if (!send_) {
      error = "NDIlib_send_create failed";
      return false;
    }
    return true;
  }

  void sendVideo(const uint8_t* data, int width, int height, int frameRateN,
                 int frameRateD) override {
    NDIlib_video_frame_v2_t frame = {};
    frame.xres = width;
    frame.yres = height;
    frame.FourCC = NDIlib_FourCC_video_type_BGRA;
    frame.frame_rate_N = frameRateN;
    frame.frame_rate_D = frameRateD;
    frame.picture_aspect_ratio = (float)width / (float)height;
    frame.frame_format_type = NDIlib_frame_format_type_progressive;
    frame.timecode = NDIlib_send_timecode_synthesize;
    frame.p_data = const_cast<uint8_t*>(data);
    frame.line_stride_in_bytes = width * 4;
    api_->send_send_video_v2(send_, &frame);
  }

  void sendAudio(const AudioFrame& audio) override {
    // The only concrete AudioFrame is the receiver's; this cast is the price
    // of keeping NDI types out of the public header.
    NDIlib_audio_frame_v3_t frame =
        static_cast<const AudioFrameImpl&>(audio).frame;
    api_->send_send_audio_v3(send_, &frame);
  }

 private:
  const NdiApi* api_ = nullptr;
  NDIlib_send_instance_t send_ = nullptr;
  std::string name_;
};

}  // namespace

std::unique_ptr<NdiReceiver> NdiReceiver::connect(
    const std::string& nameSubstring, unsigned waitMs, std::string& error) {
  auto receiver = std::make_unique<NdiReceiverImpl>();
  if (!receiver->init(nameSubstring, waitMs, error)) return nullptr;
  return receiver;
}

std::unique_ptr<NdiSender> NdiSender::create(const std::string& name,
                                             std::string& error) {
  auto sender = std::make_unique<NdiSenderImpl>();
  if (!sender->init(name, error)) return nullptr;
  return sender;
}

}  // namespace oxbow
