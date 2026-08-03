#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ffgl/ffgl_host.h"

namespace oxbow {

struct EffectSpec {
  std::string path;
  std::vector<std::pair<std::string, float>> sets;// Param name → value.
};

struct PumpOptions {
  std::string inProtocol = "ndi"; // "ndi" or "omt", each side independent.
  std::string outProtocol = "ndi";
  std::string inName;             // Source substring to receive.
  std::string outName;            // Sender name to publish.
  std::vector<EffectSpec> effects;
  unsigned discoverWaitMs = 5000;
  int controlPort = 0;            // 0 = no control server.
  std::string controlBind = "127.0.0.1";
};

/// The receive → FFGL chain → send loop, on its own thread, with live
/// parameter control from other threads.
class Pump {
 public:
  explicit Pump(PumpOptions options);
  ~Pump();
  Pump(const Pump&) = delete;
  Pump& operator=(const Pump&) = delete;

  /// Loads plugins, connects both transports, and starts the frame thread.
  /// Blocks until setup has succeeded or failed — so by the time this
  /// returns true, libomt (and its embedded .NET runtime, which replaces
  /// signal handlers) is fully initialised and the caller can safely install
  /// its own handlers.
  bool start(std::string& error);
  void stop();
  bool running() const;

  struct ParamState {
    uint32_t index = 0;
    std::string name;
    uint32_t type = 0;
    float value = 0;
    float rangeMin = 0;
    float rangeMax = 1;
  };
  struct EffectState {
    std::string name;
    std::string path;
    std::vector<ParamState> params;
  };
  struct Status {
    std::string inSource;
    uint64_t frames = 0;
    double fps = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    double frameRate = 0;
  };

  Status status() const;
  std::vector<EffectState> effects() const;

  /// Queues a set; applied on the frame thread before the next process call.
  /// False if the effect index or parameter name does not exist.
  bool setParam(size_t effectIndex, const std::string& paramName, float value);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Runs a pump (plus its control server when configured) until
/// SIGINT/SIGTERM. Returns a process exit code.
int runPump(const PumpOptions& options);

/// SIGINT/SIGTERM plumbing shared by the long-running commands. Install
/// AFTER creating any OMT object — libomt's embedded .NET runtime replaces
/// the process signal handlers when it initialises.
void installStopHandlers();
bool stopRequested();

/// Publishes a generated moving test pattern (1280x720p60) until SIGINT.
/// Exists so the whole loop can be exercised with no external tools.
int runTestSender(const std::string& protocol, const std::string& outName);

/// Receives `frames` video frames and prints resolution, rate, and a coarse
/// content fingerprint per frame. Verification tool for the other two.
/// If `dumpPath` is non-empty the first frame is written there as binary PPM.
int runProbe(const std::string& protocol, const std::string& inName,
             int frames, unsigned waitMs, const std::string& dumpPath = "");

}  // namespace oxbow
