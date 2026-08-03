// The frame pump: NDI in → FFGL chain on the GPU → NDI out.
//
// Single-threaded by design for v0.1: capture blocks up to 100 ms, then the
// frame is uploaded, processed and read back on the same thread that owns the
// GL context. At 1080p60 the whole pass is a few milliseconds; overlap can
// come later if a real workload needs it.
//
// Orientation: frames arrive bottom-up (BGRX_BGRA_flipped), which is OpenGL's
// row order, so upload is direct. NDI send has no flipped variant, so rows are
// flipped once on the way out.

#include "app/pump.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <ffgl/FFGL.h>

#include "gl/gl_headers.h"

#include "ffgl/ffgl_host.h"
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
  const EffectSpec* spec = nullptr;
};

bool rebuildInstances(std::vector<LoadedEffect>& effects, uint32_t width,
                      uint32_t height) {
  for (LoadedEffect& effect : effects) {
    effect.instance.reset();
    std::string error;
    effect.instance = effect.library->createInstance(width, height, error);
    if (!effect.instance) {
      std::fprintf(stderr, "pump: %s\n", error.c_str());
      return false;
    }
    for (const auto& [name, value] : effect.spec->sets) {
      bool found = false;
      for (const FfglParam& param : effect.library->info().params) {
        if (param.name == name) {
          effect.instance->setParamFloat(param.index, value);
          found = true;
          break;
        }
      }
      if (!found)
        std::fprintf(stderr, "pump: %s has no parameter named \"%s\"\n",
                     effect.library->info().name.c_str(), name.c_str());
    }
  }
  return true;
}

}  // namespace

int runPump(const PumpOptions& options) {
  std::string error;
  auto context = GlContext::create(error);
  if (!context || !context->makeCurrent()) {
    std::fprintf(stderr, "pump: GL context: %s\n", error.c_str());
    return 1;
  }

  std::vector<LoadedEffect> effects;
  for (const EffectSpec& spec : options.effects) {
    LoadedEffect effect;
    effect.spec = &spec;
    effect.library = FfglLibrary::open(spec.path, error);
    if (!effect.library) {
      std::fprintf(stderr, "pump: %s: %s\n", spec.path.c_str(), error.c_str());
      return 1;
    }
    std::printf("pump: loaded %s (%s)\n", effect.library->info().name.c_str(),
                spec.path.c_str());
    effects.push_back(std::move(effect));
  }

  auto receiver = connectReceiver(options.inProtocol, options.inName,
                                  options.discoverWaitMs, error);
  if (!receiver) {
    std::fprintf(stderr, "pump: %s\n", error.c_str());
    return 1;
  }
  std::printf("pump: receiving \"%s\"\n", receiver->sourceName().c_str());

  auto sender = createSender(options.outProtocol, options.outName, error);
  if (!sender) {
    std::fprintf(stderr, "pump: %s\n", error.c_str());
    return 1;
  }
  std::printf("pump: sending as \"%s\"\n", options.outName.c_str());
  installSignalHandlers();

  ChainSurfaces surfaces;
  std::vector<uint8_t> ingest;// Bottom-up, tightly packed.
  std::vector<uint8_t> readback;
  std::vector<uint8_t> sendBuffer;
  const auto start = std::chrono::steady_clock::now();
  uint64_t frames = 0;
  auto lastReport = start;

  while (!g_stop) {
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
              g_stop = true;
              return;
            }
            if (!rebuildInstances(effects, w, h)) {
              g_stop = true;
              return;
            }
            ingest.resize((size_t)w * h * 4);
            readback.resize((size_t)w * h * 4);
            sendBuffer.resize((size_t)w * h * 4);
          }

          // Normalise to bottom-up rows (OpenGL order). On Windows NDI
          // delivers them that way already; elsewhere flip once here.
          const uint8_t* rows;
          int rowStride;
          if (frame.bottomUp) {
            rows = frame.data;
            rowStride = frame.strideBytes;
          } else {
            for (uint32_t y = 0; y < h; ++y)
              std::memcpy(&ingest[(size_t)y * w * 4],
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

          const double seconds =
              std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                            start)
                  .count();

          // Chain. `previous` is the texture feeding the next effect; each
          // effect renders into the other ping-pong target. Every binding is
          // re-established per effect — plugins reset bindings to 0.
          GLuint previous = surfaces.inputTexture;
          int target = 0;
          for (LoadedEffect& effect : effects) {
            glBindFramebuffer(GL_FRAMEBUFFER, surfaces.fbo[target]);
            glViewport(0, 0, w, h);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            effect.instance->setTime(seconds);
            const bool isSource =
                effect.library->info().type == FF_SOURCE;
            if (!effect.instance->process(isSource ? 0 : previous, w, h,
                                          surfaces.fbo[target])) {
              std::fprintf(stderr, "pump: %s failed to process\n",
                           effect.library->info().name.c_str());
            }
            previous = surfaces.texture[target];
            target = 1 - target;
          }

          // Read back. Both paths end bottom-up in `readback`; with no
          // effects the normalised ingest rows pass through untouched.
          if (effects.empty()) {
            for (uint32_t y = 0; y < h; ++y)
              std::memcpy(&readback[(size_t)y * w * 4],
                          rows + (size_t)y * rowStride, (size_t)w * 4);
          } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              surfaces.fbo[1 - target]);
            glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                         readback.data());
          }

          // Flip to top-down for the sender.
          for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&sendBuffer[(size_t)y * w * 4],
                        &readback[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
          sender->sendVideo(sendBuffer.data(), w, h, frame.frameRateN,
                            frame.frameRateD, frame.timestamp);
          ++frames;
        });

    if (captured == VideoReceiver::Captured::audio) {
      if (auto audio = receiver->takeAudio()) sender->sendAudio(*audio);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(5)) {
      const double elapsed = std::chrono::duration<double>(now - start).count();
      std::printf("pump: %llu frames, %.1f fps average\n",
                  (unsigned long long)frames, frames / elapsed);
      std::fflush(stdout);
      lastReport = now;
    }
  }

  surfaces.destroy();
  std::printf("pump: stopped after %llu frames\n", (unsigned long long)frames);
  return 0;
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
    const auto next = start + frame * std::chrono::nanoseconds(16666667);
    std::this_thread::sleep_until(next);
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
