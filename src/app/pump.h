#pragma once

#include <string>
#include <utility>
#include <vector>

namespace oxbow {

struct EffectSpec {
  std::string path;
  std::vector<std::pair<std::string, float>> sets;// Param name → value.
};

struct PumpOptions {
  std::string inName;         // NDI source substring to receive.
  std::string outName;        // NDI sender name to publish.
  std::vector<EffectSpec> effects;
  unsigned discoverWaitMs = 5000;
};

/// Runs the receive → FFGL chain → send loop until SIGINT/SIGTERM.
/// Returns a process exit code.
int runPump(const PumpOptions& options);

/// Publishes a generated moving test pattern (1280x720p60) until SIGINT.
/// Exists so the whole loop can be exercised with no external NDI tools.
int runTestSender(const std::string& outName);

/// Receives `frames` video frames and prints resolution, rate, and a coarse
/// content fingerprint per frame. Verification tool for the other two.
/// If `dumpPath` is non-empty the first frame is written there as binary PPM.
int runProbe(const std::string& inName, int frames, unsigned waitMs,
             const std::string& dumpPath = "");

}  // namespace oxbow
