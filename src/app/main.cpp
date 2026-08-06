// oxbow — FFGL effects over NDI/OMT, for hosts (vMix and friends) that have
// no video plugin interface of their own.
//
// Current commands:
//   oxbow probe <plugin>     Load an FFGL plugin and print its metadata.
//   oxbow selftest <plugin>  Probe, then instantiate offscreen, run frames
//                            through it and report what came back.
//   oxbow list               Discover NDI sources on the network.
//   oxbow run --in A --out B --plugin P [--set N=V …] [--plugin …]
//                            The loop: receive A, process, publish B.
//   oxbow send-test --out N  Publish a built-in moving test pattern.
//   oxbow recv-probe --in N [--frames F]
//                            Receive F frames and print fingerprints.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ffgl/FFGL.h>

#include "gl/gl_headers.h"

#include "app/config.h"
#include "app/pump.h"
#include "diag/diag.h"
#include "ffgl/ffgl_host.h"
#include "gl/gl_context.h"
#include "io/video_io.h"

namespace oxbow {
namespace {

const char* typeName(uint32_t type) {
  switch (type) {
    case FF_EFFECT: return "effect";
    case FF_SOURCE: return "source";
    case FF_MIXER: return "mixer";
    default: return "unknown";
  }
}

void printInfo(const FfglInfo& info) {
  std::printf("name:        %s\n", info.name.c_str());
  std::printf("id:          %s\n", info.uniqueId.c_str());
  std::printf("type:        %s\n", typeName(info.type));
  std::printf("api:         %u.%u\n", info.apiMajor, info.apiMinor);
  std::printf("version:     %u.%u\n", info.versionMajor, info.versionMinor);
  if (!info.description.empty())
    std::printf("description: %s\n", info.description.c_str());
  std::printf("set-time:    %s\n", info.supportsSetTime ? "yes" : "no");
  std::printf("inputs:      %u..%u\n", info.minInputs, info.maxInputs);
  std::printf("params:      %zu\n", info.params.size());
  for (const auto& param : info.params) {
    std::printf("  [%2u] %-16s type=%-3u default=%g range=%g..%g %s",
                param.index, param.name.c_str(), param.type,
                param.defaultValue, param.rangeMin, param.rangeMax,
                param.defaultText.c_str());
    if (!param.group.empty()) std::printf(" group=%s", param.group.c_str());
    std::printf("\n");

    // Options are printed one per line rather than joined: a dropdown entry
    // may contain spaces, commas or both, and a run-together list is exactly
    // as misleading as no list at all.
    for (size_t e = 0; e < param.elements.size(); ++e) {
      std::printf("        - %-20s = %g\n", param.elements[e].c_str(),
                  e < param.elementValues.size() ? param.elementValues[e] : 0.0f);
    }
  }
}

int probe(const std::string& path) {
  std::string error;
  auto library = FfglLibrary::open(path, error);
  if (!library) {
    std::fprintf(stderr, "probe failed: %s\n", error.c_str());
    return 1;
  }
  printInfo(library->info());
  return 0;
}

int selftest(const std::string& path, const std::vector<std::string>& sets) {
  constexpr uint32_t kWidth = 1280;
  constexpr uint32_t kHeight = 720;

  std::string error;
  auto context = GlContext::create(error);
  if (!context) {
    std::fprintf(stderr, "GL context: %s\n", error.c_str());
    return 1;
  }
  if (!context->makeCurrent()) {
    std::fprintf(stderr, "GL context: makeCurrent failed\n");
    return 1;
  }
  std::printf("gl:          %s\n", context->versionString().c_str());

  auto library = FfglLibrary::open(path, error);
  if (!library) {
    std::fprintf(stderr, "probe failed: %s\n", error.c_str());
    return 1;
  }
  printInfo(library->info());
  const bool isSource = library->info().type == FF_SOURCE;

  // Input: a colour ramp with a checkerboard over the middle. The ramp gives
  // colour-mapping effects something non-constant; the checker gives
  // edge-driven effects (outline tracers, keyers) actual edges — on a smooth
  // ramp those correctly output nothing, which looks like a failure.
  std::vector<uint8_t> inputPixels(kWidth * kHeight * 4);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      uint8_t* p = &inputPixels[(y * kWidth + x) * 4];
      const bool inChecker = x > kWidth / 4 && x < 3 * kWidth / 4 &&
                             y > kHeight / 4 && y < 3 * kHeight / 4;
      if (inChecker && ((x / 80) + (y / 80)) % 2 == 0) {
        p[0] = p[1] = p[2] = 255;
      } else {
        p[0] = static_cast<uint8_t>(x * 255 / kWidth);
        p[1] = static_cast<uint8_t>(y * 255 / kHeight);
        p[2] = 128;
      }
      p[3] = 255;
    }
  }
  GLuint inputTexture = 0;
  glGenTextures(1, &inputTexture);
  glBindTexture(GL_TEXTURE_2D, inputTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, inputPixels.data());

  GLuint outputTexture = 0;
  glGenTextures(1, &outputTexture);
  glBindTexture(GL_TEXTURE_2D, outputTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);

  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         outputTexture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr, "selftest: FBO incomplete\n");
    return 1;
  }

  auto instance = library->createInstance(kWidth, kHeight, error);
  if (!instance) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  // --set overrides, matched against the probed parameter names.
  for (const auto& assignment : sets) {
    const size_t eq = assignment.find('=');
    if (eq == std::string::npos) continue;
    const std::string name = assignment.substr(0, eq);
    const float value = std::strtof(assignment.c_str() + eq + 1, nullptr);
    bool found = false;
    for (const auto& param : library->info().params) {
      if (param.name == name) {
        instance->setParamFloat(param.index, value);
        std::printf("set:         %s = %g\n", name.c_str(), value);
        found = true;
        break;
      }
    }
    if (!found) std::fprintf(stderr, "set: no parameter named %s\n", name.c_str());
  }

  // A couple of seconds of frames so time-driven plugins (rain, scan lines)
  // have visibly started before we look at the output.
  constexpr int kFrames = 120;
  for (int frame = 0; frame < kFrames; ++frame) {
    // Rebind everything every frame: FFGL plugins routinely leave bindings
    // reset to 0 on the way out.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, kWidth, kHeight);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    instance->setTime(frame / 60.0);
    if (!instance->process(isSource ? 0 : inputTexture, kWidth, kHeight,
                           fbo)) {
      std::fprintf(stderr, "selftest: FF_PROCESS_OPENGL failed on frame %d\n",
                   frame);
      return 1;
    }
  }

  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  std::vector<uint8_t> outputPixels(kWidth * kHeight * 4);
  glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE,
               outputPixels.data());

  size_t litPixels = 0;
  uint64_t sum = 0;
  for (size_t i = 0; i < outputPixels.size(); i += 4) {
    if (outputPixels[i] || outputPixels[i + 1] || outputPixels[i + 2])
      ++litPixels;
    sum += outputPixels[i] + outputPixels[i + 1] + outputPixels[i + 2];
  }
  const GLenum glError = glGetError();
  std::printf("frames:      %d\n", kFrames);
  std::printf("lit pixels:  %zu of %u (%.1f%%)\n", litPixels, kWidth * kHeight,
              100.0 * litPixels / (kWidth * kHeight));
  std::printf("rgb sum:     %llu\n", static_cast<unsigned long long>(sum));
  std::printf("gl error:    0x%x\n", glError);
  const bool pass = litPixels > 0 && glError == GL_NO_ERROR;
  std::printf("selftest:    %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

int listSourcesCommand(const std::string& protocol) {
  std::string error;
  auto sources = listSources(protocol, 3000, error);
  if (!error.empty()) {
    std::fprintf(stderr, "list: %s\n", error.c_str());
    return 1;
  }
  for (const auto& source : sources)
    std::printf("%s\t%s\n", source.name.c_str(), source.url.c_str());
  std::printf("%zu source(s)\n", sources.size());
  return 0;
}

}  // namespace
}  // namespace oxbow

int main(int argc, char** argv) {
  using oxbow::EffectSpec;
  using oxbow::PumpOptions;

  {
    // Logging up before anything that can fail. The crash handler waits:
    // libomt's embedded .NET runtime replaces process handlers when it
    // initialises (same trap as CEF), so runPump installs it after the
    // transports exist.
    oxbow::diag::Options diagOptions;
    diagOptions.appName = "oxbow";
    diagOptions.envPrefix = "OXBOW";
#if defined(OXBOW_VERSION)
    diagOptions.version = OXBOW_VERSION;
#endif
    diagOptions.installCrashHandler = false;
    oxbow::diag::init(diagOptions);
  }

  auto nextArg = [&](int& i) -> const char* {
    return i + 1 < argc ? argv[++i] : nullptr;
  };

  if (argc >= 2 && std::strcmp(argv[1], "list") == 0) {
    std::string proto = "ndi";
    for (int i = 2; i < argc; ++i)
      if (std::strcmp(argv[i], "--proto") == 0)
        if (const char* v = nextArg(i)) proto = v;
    return oxbow::listSourcesCommand(proto);
  }

  if (argc >= 2 && std::strcmp(argv[1], "send-test") == 0) {
    std::string out = "oxbow-test";
    std::string proto = "ndi";
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--out") == 0) {
        if (const char* v = nextArg(i)) out = v;
      } else if (std::strcmp(argv[i], "--proto") == 0) {
        if (const char* v = nextArg(i)) proto = v;
      }
    }
    return oxbow::runTestSender(proto, out);
  }

  if (argc >= 2 && std::strcmp(argv[1], "recv-probe") == 0) {
    std::string in;
    std::string dump;
    std::string proto = "ndi";
    int frames = 30;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--in") == 0) {
        if (const char* v = nextArg(i)) in = v;
      } else if (std::strcmp(argv[i], "--frames") == 0) {
        if (const char* v = nextArg(i)) frames = std::atoi(v);
      } else if (std::strcmp(argv[i], "--dump") == 0) {
        if (const char* v = nextArg(i)) dump = v;
      } else if (std::strcmp(argv[i], "--proto") == 0) {
        if (const char* v = nextArg(i)) proto = v;
      }
    }
    if (in.empty()) {
      std::fprintf(stderr, "recv-probe: --in is required\n");
      return 2;
    }
    return oxbow::runProbe(proto, in, frames, 5000, dump);
  }

  if (argc >= 2 && std::strcmp(argv[1], "run") == 0) {
    PumpOptions options;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--config") == 0) {
        const char* v = nextArg(i);
        std::string error;
        if (!v || !oxbow::loadConfig(v, options, error)) {
          std::fprintf(stderr, "run: %s\n",
                       v ? error.c_str() : "--config needs a path");
          return 2;
        }
      } else if (std::strcmp(argv[i], "--in") == 0) {
        if (const char* v = nextArg(i)) options.inName = v;
      } else if (std::strcmp(argv[i], "--out") == 0) {
        if (const char* v = nextArg(i)) options.outName = v;
      } else if (std::strcmp(argv[i], "--in-proto") == 0) {
        if (const char* v = nextArg(i)) options.inProtocol = v;
      } else if (std::strcmp(argv[i], "--out-proto") == 0) {
        if (const char* v = nextArg(i)) options.outProtocol = v;
      } else if (std::strcmp(argv[i], "--port") == 0) {
        if (const char* v = nextArg(i)) options.controlPort = std::atoi(v);
      } else if (std::strcmp(argv[i], "--bind") == 0) {
        if (const char* v = nextArg(i)) options.controlBind = v;
      } else if (std::strcmp(argv[i], "--plugin") == 0) {
        if (const char* v = nextArg(i)) {
          EffectSpec spec;
          spec.path = v;
          options.effects.push_back(spec);
        }
      } else if (std::strcmp(argv[i], "--set") == 0) {
        const char* v = nextArg(i);
        if (v && !options.effects.empty()) {
          const char* eq = std::strchr(v, '=');
          if (eq)
            options.effects.back().sets.emplace_back(
                std::string(v, eq - v), std::strtof(eq + 1, nullptr));
        }
      }
    }
    if (options.inName.empty() || options.outName.empty()) {
      std::fprintf(stderr, "run: --in and --out are required\n");
      return 2;
    }
    return oxbow::runPump(options);
  }

  if (argc == 3 && std::strcmp(argv[1], "probe") == 0)
    return oxbow::probe(argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "selftest") == 0) {
    std::vector<std::string> sets;
    for (int i = 3; i + 1 < argc; i += 2) {
      if (std::strcmp(argv[i], "--set") == 0) sets.push_back(argv[i + 1]);
    }
    return oxbow::selftest(argv[2], sets);
  }
  std::fprintf(stderr,
               "usage: oxbow probe <plugin>\n"
               "       oxbow selftest <plugin> [--set Name=value ...]\n"
               "       oxbow list [--proto ndi|omt]\n"
               "       oxbow run --config <file.json>\n"
               "       oxbow run --in <source> --out <name> [--in-proto ndi|omt]\n"
               "                 [--out-proto ndi|omt] [--plugin <path> [--set N=V ...]]...\n"
               "       oxbow send-test [--out <name>] [--proto ndi|omt]\n"
               "       oxbow recv-probe --in <source> [--frames <n>] [--proto ndi|omt] [--dump <ppm>]\n");
  return 2;
}
