#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/dylib.h"

namespace oxbow {

/// One FFGL parameter, as reported by the plugin's prototype instance.
struct FfglParam {
  uint32_t index = 0;
  std::string name;
  uint32_t type = 0;      // FF_TYPE_*
  float defaultValue = 0; // For float-like types.
  std::string defaultText;// For FF_TYPE_TEXT / FF_TYPE_FILE.
  float rangeMin = 0;     // FF_GET_RANGE; 0..1 when unsupported.
  float rangeMax = 1;

  /// The plugin's own grouping for this parameter, or empty. Hosts that can
  /// show sections (Resolume, OBS) use it; the CLI ignores it.
  std::string group;

  /// FF_TYPE_OPTION only — the dropdown's entries and the value each one
  /// stands for. Empty for every other type, deliberately: the SDK answers
  /// FF_GET_NUM_PARAMETER_ELEMENTS with 1 for *any* parameter, so a host that
  /// does not gate on the type turns every slider into a one-item dropdown.
  std::vector<std::string> elements;
  std::vector<float> elementValues;
};

struct FfglInfo {
  std::string uniqueId;   // 4 chars.
  std::string name;       // Up to 16 chars.
  uint32_t type = 0;      // FF_EFFECT / FF_SOURCE / FF_MIXER.
  uint32_t apiMajor = 0;
  uint32_t apiMinor = 0;
  uint32_t versionMajor = 0;
  uint32_t versionMinor = 0;
  std::string description;
  std::string about;
  bool supportsSetTime = false;
  bool topLeftOrientation = false;
  uint32_t minInputs = 0;
  uint32_t maxInputs = 0;
  std::vector<FfglParam> params;
};

class FfglInstance;

/// A loaded FFGL plugin library: the shared object plus its initialised
/// prototype. GL-touching calls (createInstance and everything on
/// FfglInstance) require the host GL context current on the calling thread.
class FfglLibrary {
 public:
  ~FfglLibrary();
  FfglLibrary(const FfglLibrary&) = delete;
  FfglLibrary& operator=(const FfglLibrary&) = delete;

  /// `path` is a .bundle directory (macOS) or .dll (Windows). Calls
  /// FF_INITIALISE_V2, which the SDK requires before any metadata query —
  /// plugMain answers FF_GET_* from a prototype instance that only exists
  /// after initialise.
  static std::unique_ptr<FfglLibrary> open(const std::string& path,
                                           std::string& error);

  const FfglInfo& info() const { return info_; }
  const std::string& path() const { return path_; }

  /// Instantiates the plugin at the given output size. GL context required.
  std::unique_ptr<FfglInstance> createInstance(uint32_t width, uint32_t height,
                                               std::string& error);

 private:
  friend class FfglInstance;
  FfglLibrary() = default;
  bool probe(std::string& error);

  Dylib dylib_;
  void* plugMain_ = nullptr;
  std::string path_;
  FfglInfo info_;
  bool initialised_ = false;
};

/// A live plugin instance bound to one resolution.
///
/// After process() assume every GL binding is trashed: plugins built on the
/// stock SDK "restore" bindings to 0 rather than the previous value (the
/// Scoped* helpers in the SDK do this), so the host rebinds all state it
/// needs each frame instead of caching bindings across calls.
class FfglInstance {
 public:
  ~FfglInstance();
  FfglInstance(const FfglInstance&) = delete;
  FfglInstance& operator=(const FfglInstance&) = delete;

  bool setParamFloat(uint32_t index, float value);
  bool setParamText(uint32_t index, const std::string& value);
  bool setTime(double seconds);

  /// Runs the plugin. `inputTexture` 0 means no input (sources). The output
  /// lands in whatever framebuffer `hostFbo` names; the host must have it
  /// bound before the call and must rebind everything after.
  bool process(uint32_t inputTexture, uint32_t width, uint32_t height,
               uint32_t hostFbo);

 private:
  friend class FfglLibrary;
  FfglInstance(FfglLibrary* library, void* instanceId, uint32_t width,
               uint32_t height)
      : library_(library), instanceId_(instanceId), width_(width),
        height_(height) {}

  FfglLibrary* library_ = nullptr;
  void* instanceId_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

}  // namespace oxbow
