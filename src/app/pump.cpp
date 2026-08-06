// The frame pump: video in → FFGL chain on the GPU → video out.
//
// One thread owns everything GL: the context, the FFGL instances, and the
// frame loop. Other threads talk to it through a mutex-protected queue of
// parameter sets and a snapshot of status/effect state. Single-threaded
// frame handling is deliberate for v0.1: at 1080p60 the whole pass is a few
// milliseconds; overlap can come later if a real workload needs it.
//
// Orientation: rows are normalised to bottom-up (OpenGL order) at ingest —
// free on Windows NDI (the _flipped receive format), one CPU pass elsewhere —
// and flipped back once on the way out.

#include "app/pump.h"

#include "app/main_loop.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <ffgl/FFGL.h>

#include "gl/gl_headers.h"

#include "gl/gl_context.h"
#include "io/video_io.h"

namespace oxbow {
namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

// Installed AFTER the transports are created: libomt embeds the .NET
// runtime, which registers its own SIGINT/SIGTERM handling when it starts.
// Registering first means being replaced — the process then ignores Ctrl-C
// and lingers holding the OMT listen port.
void installSignalHandlers() {
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
}

/// GPU-side state that depends on the video resolution; rebuilt whenever the
/// incoming size changes.
struct ChainSurfaces {
  uint32_t width = 0;
  uint32_t height = 0;
  GLuint inputTexture = 0;
  GLuint texture[2] = {0, 0};// Ping-pong colour targets.
  GLuint fbo[2] = {0, 0};

  void destroy() {
    glDeleteFramebuffers(2, fbo);
    glDeleteTextures(2, texture);
    glDeleteTextures(1, &inputTexture);
    *this = ChainSurfaces();
  }

  bool build(uint32_t w, uint32_t h) {
    destroy();
    width = w;
    height = h;
    glGenTextures(1, &inputTexture);
    glBindTexture(GL_TEXTURE_2D, inputTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    glGenTextures(2, texture);
    glGenFramebuffers(2, fbo);
    for (int i = 0; i < 2; ++i) {
      glBindTexture(GL_TEXTURE_2D, texture[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA,
                   GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, texture[i], 0);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;
    }
    return true;
  }
};

struct LoadedEffect {
  std::unique_ptr<FfglLibrary> library;
  std::unique_ptr<FfglInstance> instance;
  EffectSpec spec;
  std::vector<float> values;// Current float value per param index.
};

}  // namespace

void installStopHandlers() { installSignalHandlers(); }
bool stopRequested() { return g_stop; }

struct Pump::Impl {
  PumpOptions options;

  std::thread thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> alive{false};

  // Setup handshake: the frame thread reports one-time setup success/failure.
  std::mutex setupMutex;
  std::condition_variable setupCv;
  bool setupDone = false;
  std::string setupError;

  // Cross-thread state. `mutex` guards pendingSets and the mirrored effect
  // values; the frame thread drains pendingSets each frame.
  mutable std::mutex mutex;
  struct PendingSet {
    size_t effect;
    uint32_t param;
    float value;
  };
  std::vector<PendingSet> pendingSets;
  std::vector<LoadedEffect> effects;// Owned by the frame thread after start.

  std::atomic<uint64_t> frames{0};
  std::atomic<uint32_t> width{0};
  std::atomic<uint32_t> height{0};
  std::atomic<double> frameRate{0};
  std::atomic<double> fps{0};
  std::string inSource;// Written once during setup.

  void finishSetup(const std::string& error) {
    std::lock_guard<std::mutex> lock(setupMutex);
    setupError = error;
    setupDone = true;
    setupCv.notify_all();
  }

  void threadMain() {
    std::string error;
    auto context = GlContext::create(error);
    if (!context || !context->makeCurrent()) {
      finishSetup("GL context: " + error);
      return;
    }

    for (const EffectSpec& spec : options.effects) {
      LoadedEffect effect;
      effect.spec = spec;
      effect.library = FfglLibrary::open(spec.path, error);
      if (!effect.library) {
        finishSetup(spec.path + ": " + error);
        return;
      }
      effect.values.resize(effect.library->info().params.size());
      for (const FfglParam& param : effect.library->info().params)
        effect.values[param.index] = param.defaultValue;
      std::printf("pump: loaded %s (%s)\n",
                  effect.library->info().name.c_str(), spec.path.c_str());
      effects.push_back(std::move(effect));
    }

    auto receiver = connectReceiver(options.inProtocol, options.inName,
                                    options.discoverWaitMs, error);
    if (!receiver) {
      finishSetup(error);
      return;
    }
    inSource = receiver->sourceName();
    std::printf("pump: receiving \"%s\"\n", inSource.c_str());

    auto sender = createSender(options.outProtocol, options.outName, error);
    if (!sender) {
      finishSetup(error);
      return;
    }
    std::printf("pump: sending as \"%s\"\n", options.outName.c_str());

    alive = true;
    finishSetup("");

    ChainSurfaces surfaces;
    std::vector<uint8_t> ingest;// Bottom-up, tightly packed.
    std::vector<uint8_t> readback;
    std::vector<uint8_t> sendBuffer;
    const auto start = std::chrono::steady_clock::now();
    auto windowStart = start;
    uint64_t windowFrames = 0;

    while (!stop) {
      const VideoReceiver::Captured captured = receiver->capture(
          100, [&](const VideoFrame& frame) {
            const uint32_t w = (uint32_t)frame.width;
            const uint32_t h = (uint32_t)frame.height;
            if (w == 0 || h == 0) return;

            if (surfaces.width != w || surfaces.height != h) {
              std::printf("pump: video is %ux%u @ %g\n", w, h,
                          (double)frame.frameRateN / frame.frameRateD);
              if (!surfaces.build(w, h)) {
                std::fprintf(stderr, "pump: FBO incomplete at %ux%u\n", w, h);
                stop = true;
                return;
              }
              if (!rebuildInstances(w, h)) {
                stop = true;
                return;
              }
              ingest.resize((size_t)w * h * 4);
              readback.resize((size_t)w * h * 4);
              sendBuffer.resize((size_t)w * h * 4);
              width = w;
              height = h;
              frameRate = (double)frame.frameRateN / frame.frameRateD;
            }

            applyPendingSets();

            // Normalise to bottom-up rows (OpenGL order). Windows NDI
            // delivers them that way already; elsewhere flip once here.
            const uint8_t* rows;
            int rowStride;
            if (frame.bottomUp) {
              rows = frame.data;
              rowStride = frame.strideBytes;
            } else {
              for (uint32_t y = 0; y < h; ++y)
                std::memcpy(
                    &ingest[(size_t)y * w * 4],
                    frame.data + (size_t)(h - 1 - y) * frame.strideBytes,
                    (size_t)w * 4);
              rows = ingest.data();
              rowStride = (int)w * 4;
            }

            // Upload. Stride can exceed w*4; ROW_LENGTH covers it.
            glBindTexture(GL_TEXTURE_2D, surfaces.inputTexture);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rowStride / 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA,
                            GL_UNSIGNED_INT_8_8_8_8_REV, rows);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

            const double seconds = std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - start)
                                       .count();

            // Chain. `previous` feeds the next effect; each renders into the
            // other ping-pong target. Every binding is re-established per
            // effect — plugins reset bindings to 0.
            GLuint previous = surfaces.inputTexture;
            int target = 0;
            for (LoadedEffect& effect : effects) {
              glBindFramebuffer(GL_FRAMEBUFFER, surfaces.fbo[target]);
              glViewport(0, 0, w, h);
              glClearColor(0, 0, 0, 0);
              glClear(GL_COLOR_BUFFER_BIT);
              effect.instance->setTime(seconds);
              const bool isSource = effect.library->info().type == FF_SOURCE;
              if (!effect.instance->process(isSource ? 0 : previous, w, h,
                                            surfaces.fbo[target])) {
                std::fprintf(stderr, "pump: %s failed to process\n",
                             effect.library->info().name.c_str());
              }
              previous = surfaces.texture[target];
              target = 1 - target;
            }

            // Read back; both paths end bottom-up in `readback`.
            if (effects.empty()) {
              for (uint32_t y = 0; y < h; ++y)
                std::memcpy(&readback[(size_t)y * w * 4],
                            rows + (size_t)y * rowStride, (size_t)w * 4);
            } else {
              glBindFramebuffer(GL_READ_FRAMEBUFFER, surfaces.fbo[1 - target]);
              glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                           readback.data());
            }

            // Flip to top-down for the sender.
            for (uint32_t y = 0; y < h; ++y)
              std::memcpy(&sendBuffer[(size_t)y * w * 4],
                          &readback[(size_t)(h - 1 - y) * w * 4],
                          (size_t)w * 4);
            sender->sendVideo(sendBuffer.data(), w, h, frame.frameRateN,
                              frame.frameRateD, frame.timestamp);
            ++frames;
            ++windowFrames;
          });

      if (captured == VideoReceiver::Captured::audio) {
        if (auto audio = receiver->takeAudio()) sender->sendAudio(*audio);
      }

      const auto now = std::chrono::steady_clock::now();
      const double window =
          std::chrono::duration<double>(now - windowStart).count();
      if (window >= 2.0) {
        fps = windowFrames / window;
        windowFrames = 0;
        windowStart = now;
      }
    }

    surfaces.destroy();
    effects.clear();// Instances die with the GL context still current.
    alive = false;
  }

  bool rebuildInstances(uint32_t w, uint32_t h) {
    for (LoadedEffect& effect : effects) {
      effect.instance.reset();
      std::string error;
      effect.instance = effect.library->createInstance(w, h, error);
      if (!effect.instance) {
        std::fprintf(stderr, "pump: %s\n", error.c_str());
        return false;
      }
      // Config-file sets, then anything adjusted live since.
      for (const auto& [name, value] : effect.spec.sets)
        setByName(effect, name, value);
      for (const FfglParam& param : effect.library->info().params) {
        if (effect.values[param.index] != param.defaultValue)
          effect.instance->setParamFloat(param.index,
                                         effect.values[param.index]);
      }
    }
    return true;
  }

  void setByName(LoadedEffect& effect, const std::string& name, float value) {
    for (const FfglParam& param : effect.library->info().params) {
      if (param.name == name) {
        effect.instance->setParamFloat(param.index, value);
        effect.values[param.index] = value;
        return;
      }
    }
    std::fprintf(stderr, "pump: %s has no parameter named \"%s\"\n",
                 effect.library->info().name.c_str(), name.c_str());
  }

  void applyPendingSets() {
    std::vector<PendingSet> sets;
    {
      std::lock_guard<std::mutex> lock(mutex);
      sets.swap(pendingSets);
    }
    for (const PendingSet& set : sets) {
      if (set.effect >= effects.size()) continue;
      LoadedEffect& effect = effects[set.effect];
      if (!effect.instance || set.param >= effect.values.size()) continue;
      effect.instance->setParamFloat(set.param, set.value);
      std::lock_guard<std::mutex> lock(mutex);
      effect.values[set.param] = set.value;
    }
  }
};

Pump::Pump(PumpOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->options = std::move(options);
}

Pump::~Pump() { stop(); }

bool Pump::start(std::string& error) {
  impl_->thread = std::thread([this] { impl_->threadMain(); });
  std::unique_lock<std::mutex> lock(impl_->setupMutex);
  impl_->setupCv.wait(lock, [this] { return impl_->setupDone; });
  if (!impl_->setupError.empty()) {
    error = impl_->setupError;
    lock.unlock();
    impl_->thread.join();
    return false;
  }
  return true;
}

void Pump::stop() {
  impl_->stop = true;
  if (impl_->thread.joinable()) impl_->thread.join();
}

bool Pump::running() const { return impl_->alive; }

Pump::Status Pump::status() const {
  Status status;
  status.inSource = impl_->inSource;
  status.frames = impl_->frames;
  status.fps = impl_->fps;
  status.width = impl_->width;
  status.height = impl_->height;
  status.frameRate = impl_->frameRate;
  return status;
}

std::vector<Pump::EffectState> Pump::effects() const {
  std::vector<EffectState> result;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const LoadedEffect& effect : impl_->effects) {
    EffectState state;
    state.name = effect.library->info().name;
    state.path = effect.spec.path;
    for (const FfglParam& param : effect.library->info().params) {
      ParamState paramState;
      paramState.index = param.index;
      paramState.name = param.name;
      paramState.type = param.type;
      paramState.value = effect.values[param.index];
      paramState.rangeMin = param.rangeMin;
      paramState.rangeMax = param.rangeMax;
      state.params.push_back(std::move(paramState));
    }
    result.push_back(std::move(state));
  }
  return result;
}

bool Pump::setParam(size_t effectIndex, const std::string& paramName,
                    float value) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (effectIndex >= impl_->effects.size()) return false;
  const LoadedEffect& effect = impl_->effects[effectIndex];
  for (const FfglParam& param : effect.library->info().params) {
    if (param.name == paramName) {
      impl_->pendingSets.push_back({effectIndex, param.index, value});
      return true;
    }
  }
  return false;
}

int runTestSender(const std::string& protocol, const std::string& outName) {
  std::string error;
  auto sender = createSender(protocol, outName, error);
  if (!sender) {
    std::fprintf(stderr, "send-test: %s\n", error.c_str());
    return 1;
  }
  installSignalHandlers();
  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  std::vector<uint8_t> pixels((size_t)kWidth * kHeight * 4);
  std::printf("send-test: publishing \"%s\" %dx%d@60\n", outName.c_str(),
              kWidth, kHeight);

  uint64_t frame = 0;
  const auto start = std::chrono::steady_clock::now();
  while (!g_stop) {
    // Colour bars with a moving vertical white sweep: static structure for
    // the eye, per-frame change for the probe's fingerprint.
    const int sweep = (int)((frame * 4) % kWidth);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        uint8_t* p = &pixels[((size_t)y * kWidth + x) * 4];
        const int bar = x * 8 / kWidth;
        const uint8_t r = (bar & 1) ? 191 : 0;
        const uint8_t g = (bar & 2) ? 191 : 0;
        const uint8_t b = (bar & 4) ? 191 : 0;
        const bool inSweep = x >= sweep && x < sweep + 40;
        p[0] = inSweep ? 255 : b;
        p[1] = inSweep ? 255 : g;
        p[2] = inSweep ? 255 : r;
        p[3] = 255;
      }
    }
    sender->sendVideo(pixels.data(), kWidth, kHeight, 60000, 1000, -1);
    ++frame;
    // Paced by servicing the main run loop rather than sleeping on it: this
    // loop *is* the main thread, and a Syphon output dispatches its server
    // creation here. See app/main_loop.h.
    const auto next = start + frame * std::chrono::nanoseconds(16666667);
    const double remaining =
        std::chrono::duration<double>(next - std::chrono::steady_clock::now()).count();
    waitServicingMainLoop(remaining);
  }
  std::printf("send-test: stopped after %llu frames\n",
              (unsigned long long)frame);
  return 0;
}

int runProbe(const std::string& protocol, const std::string& inName,
             int frames, unsigned waitMs, const std::string& dumpPath) {
  std::string error;
  auto receiver = connectReceiver(protocol, inName, waitMs, error);
  if (!receiver) {
    std::fprintf(stderr, "recv-probe: %s\n", error.c_str());
    return 1;
  }
  std::printf("recv-probe: connected to \"%s\"\n",
              receiver->sourceName().c_str());

  int received = 0;
  int idleMs = 0;
  while (received < frames && idleMs < 10000) {
    const auto captured = receiver->capture(250, [&](const VideoFrame& frame) {
      // Coarse fingerprint: the mean of every 1009th byte. Any effect that
      // changes the picture changes it; a static picture repeats it.
      uint64_t sum = 0;
      size_t samples = 0;
      const size_t total = (size_t)frame.height * frame.strideBytes;
      for (size_t i = 0; i < total; i += 1009) {
        sum += frame.data[i];
        ++samples;
      }
      std::printf("frame %3d: %dx%d @ %g stride=%d mean=%llu\n", received,
                  frame.width, frame.height,
                  (double)frame.frameRateN / frame.frameRateD,
                  frame.strideBytes,
                  (unsigned long long)(samples ? sum / samples : 0));
      if (received == 0 && !dumpPath.empty()) {
        if (FILE* file = std::fopen(dumpPath.c_str(), "wb")) {
          std::fprintf(file, "P6\n%d %d\n255\n", frame.width, frame.height);
          for (int y = 0; y < frame.height; ++y) {
            const int row = frame.bottomUp ? frame.height - 1 - y : y;
            const uint8_t* p = frame.data + (size_t)row * frame.strideBytes;
            for (int x = 0; x < frame.width; ++x, p += 4) {
              const uint8_t rgb[3] = {p[2], p[1], p[0]};// BGRA → RGB.
              std::fwrite(rgb, 1, 3, file);
            }
          }
          std::fclose(file);
          std::printf("recv-probe: wrote %s\n", dumpPath.c_str());
        }
      }
      ++received;
    });
    idleMs = captured == VideoReceiver::Captured::none ? idleMs + 250 : 0;
  }
  return received >= frames ? 0 : 1;
}

}  // namespace oxbow
