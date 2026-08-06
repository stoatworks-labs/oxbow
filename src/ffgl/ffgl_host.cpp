// FFGL 2.x host.
//
// The plugin surface is one C entry point, plugMain(functionCode, input,
// instanceID). Metadata calls go to a prototype instance the library creates
// during FF_INITIALISE_V2; rendering calls go to per-instance state created by
// FF_INSTANTIATE_GL. Floats cross the ABI bit-cast into a 32-bit integer
// (FFMixed.UIntValue), never by pointer.

#include "ffgl/ffgl_host.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

#include <ffgl/FFGL.h>

namespace oxbow {
namespace {

using PlugMainFn = FFMixed (*)(FFUInt32, FFMixed, FFInstanceID);

FFMixed uintMixed(FFUInt32 value) {
  FFMixed m;
  m.UIntValue = value;
  return m;
}

FFMixed pointerMixed(void* value) {
  FFMixed m;
  m.PointerValue = value;
  return m;
}

float mixedToFloat(FFMixed m) {
  float f;
  static_assert(sizeof f == sizeof m.UIntValue, "FFGL float ABI");
  std::memcpy(&f, &m.UIntValue, sizeof f);
  return f;
}

FFMixed floatToMixed(float f) {
  FFMixed m;
  m.UIntValue = 0;
  std::memcpy(&m.UIntValue, &f, sizeof f);
  return m;
}

/// Fixed-width, non-NUL-terminated char fields (PluginName, parameter names).
std::string fixedString(const char* data, size_t maxLength) {
  size_t length = 0;
  while (length < maxLength && data[length] != '\0') ++length;
  return std::string(data, length);
}

bool isTextualType(uint32_t type) {
  return type == FF_TYPE_TEXT || type == FF_TYPE_FILE;
}

/// Resolves the actual shared object to dlopen. On macOS an FFGL plugin is a
/// .bundle directory; the binary lives in Contents/MacOS and is normally the
/// only file there, so take the first regular file rather than guessing at
/// name conventions.
std::string resolveBinaryPath(const std::string& path, std::string& error) {
#if defined(__APPLE__)
  namespace fs = std::filesystem;
  fs::path p(path);
  if (fs::is_directory(p)) {
    fs::path macos = p / "Contents" / "MacOS";
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(macos, ec)) {
      if (entry.is_regular_file()) return entry.path().string();
    }
    error = "no binary found under " + macos.string();
    return "";
  }
#endif
  return path;
}

}  // namespace

FfglLibrary::~FfglLibrary() {
  if (initialised_ && plugMain_) {
    reinterpret_cast<PlugMainFn>(plugMain_)(FF_DEINITIALISE, uintMixed(0),
                                            nullptr);
  }
}

std::unique_ptr<FfglLibrary> FfglLibrary::open(const std::string& path,
                                               std::string& error) {
  std::string binary = resolveBinaryPath(path, error);
  if (binary.empty()) return nullptr;

  std::unique_ptr<FfglLibrary> library(new FfglLibrary());
  library->path_ = path;
  if (!library->dylib_.open({binary})) {
    error = library->dylib_.lastError();
    return nullptr;
  }
  library->plugMain_ = library->dylib_.rawSymbol("plugMain");
  if (!library->plugMain_) {
    error = binary + " exports no plugMain — not an FFGL plugin";
    return nullptr;
  }

  // Optional second export: SDK-built plugins route FFGLLog::LogToHost through
  // a callback registered here. Shader compile failures land on it, which
  // turns "FF_INSTANTIATE_GL failed" into an actual diagnosis.
  if (void* setLog = library->dylib_.rawSymbol("SetLogCallback")) {
    reinterpret_cast<void (*)(PFNLog)>(setLog)([](char* message) {
      std::fprintf(stderr, "[ffgl] %s\n", message ? message : "");
    });
  }

  auto call = reinterpret_cast<PlugMainFn>(library->plugMain_);
  if (call(FF_INITIALISE_V2, uintMixed(0), nullptr).UIntValue != FF_SUCCESS) {
    error = "FF_INITIALISE_V2 failed";
    return nullptr;
  }
  library->initialised_ = true;

  if (!library->probe(error)) return nullptr;
  return library;
}

bool FfglLibrary::probe(std::string& error) {
  auto call = reinterpret_cast<PlugMainFn>(plugMain_);

  auto* pluginInfo = static_cast<PluginInfoStruct*>(
      call(FF_GET_INFO, uintMixed(0), nullptr).PointerValue);
  if (!pluginInfo) {
    error = "FF_GET_INFO returned nothing";
    return false;
  }
  info_.apiMajor = pluginInfo->APIMajorVersion;
  info_.apiMinor = pluginInfo->APIMinorVersion;
  info_.uniqueId = fixedString(pluginInfo->PluginUniqueID, 4);
  info_.name = fixedString(pluginInfo->PluginName, 16);
  info_.type = pluginInfo->PluginType;

  auto* extended = static_cast<PluginExtendedInfoStruct*>(
      call(FF_GET_EXTENDED_INFO, uintMixed(0), nullptr).PointerValue);
  if (extended) {
    info_.versionMajor = extended->PluginMajorVersion;
    info_.versionMinor = extended->PluginMinorVersion;
    if (extended->Description) info_.description = extended->Description;
    if (extended->About) info_.about = extended->About;
  }

  info_.supportsSetTime =
      call(FF_GET_PLUGIN_CAPS, uintMixed(FF_CAP_SET_TIME), nullptr).UIntValue ==
      FF_SUPPORTED;
  info_.topLeftOrientation =
      call(FF_GET_PLUGIN_CAPS, uintMixed(FF_CAP_TOP_LEFT_TEXTURE_ORIENTATION),
           nullptr)
          .UIntValue == FF_SUPPORTED;
  info_.minInputs =
      call(FF_GET_PLUGIN_CAPS, uintMixed(FF_CAP_MINIMUM_INPUT_FRAMES), nullptr)
          .UIntValue;
  info_.maxInputs =
      call(FF_GET_PLUGIN_CAPS, uintMixed(FF_CAP_MAXIMUM_INPUT_FRAMES), nullptr)
          .UIntValue;

  const uint32_t numParams =
      call(FF_GET_NUM_PARAMETERS, uintMixed(0), nullptr).UIntValue;
  for (uint32_t i = 0; i < numParams; ++i) {
    FfglParam param;
    param.index = i;
    const char* name = static_cast<const char*>(
        call(FF_GET_PARAMETER_NAME, uintMixed(i), nullptr).PointerValue);
    if (name) param.name = fixedString(name, 16);
    param.type = call(FF_GET_PARAMETER_TYPE, uintMixed(i), nullptr).UIntValue;

    FFMixed def = call(FF_GET_PARAMETER_DEFAULT, uintMixed(i), nullptr);
    if (isTextualType(param.type)) {
      if (def.PointerValue)
        param.defaultText = static_cast<const char*>(def.PointerValue);
    } else {
      param.defaultValue = mixedToFloat(def);
    }

    GetRangeStruct range = {};
    range.parameterNumber = i;
    range.range.min = 0;
    range.range.max = 1;
    if (call(FF_GET_RANGE, pointerMixed(&range), nullptr).UIntValue ==
        FF_SUCCESS) {
      param.rangeMin = range.range.min;
      param.rangeMax = range.range.max;
    }

    // FF_GET_PARAM_GROUP is a fill-my-buffer call, not a give-me-a-pointer
    // one: the host supplies the storage and the plugin memcpys into it,
    // **without** a terminating nul. Its neighbours all return a char*, so
    // the two conventions look identical at the call site — and passing an
    // index where the struct pointer belongs is not a type error inside an
    // FFMixed. It compiles, then reads maxToWrite from address 11.
    {
      char groupBuffer[64] = {};
      GetStringStruct getGroup = {};
      getGroup.parameterNumber = i;
      getGroup.stringBuffer.address = groupBuffer;
      getGroup.stringBuffer.maxToWrite = sizeof(groupBuffer) - 1;
      if (call(FF_GET_PARAM_GROUP, pointerMixed(&getGroup), nullptr).UIntValue ==
          FF_SUCCESS) {
        param.group = groupBuffer;
      }
    }

    // Gate on FF_TYPE_OPTION. FF_GET_NUM_PARAMETER_ELEMENTS answers **1 for
    // every parameter** — the SDK keeps the current value as element 0 — so
    // asking every parameter would make each one look like a one-entry
    // dropdown.
    if (param.type == FF_TYPE_OPTION) {
      const uint32_t elements =
          call(FF_GET_NUM_PARAMETER_ELEMENTS, uintMixed(i), nullptr).UIntValue;
      for (uint32_t e = 0; e < elements; ++e) {
        GetParameterElementNameStruct elementName = {i, e};
        const char* text = static_cast<const char*>(
            call(FF_GET_PARAMETER_ELEMENT_NAME, pointerMixed(&elementName),
                 nullptr)
                .PointerValue);
        param.elements.push_back(text ? text
                                      : "Option " + std::to_string(e));

        GetParameterElementValueStruct elementValue = {i, e};
        param.elementValues.push_back(mixedToFloat(call(
            FF_GET_PARAMETER_ELEMENT_VALUE, pointerMixed(&elementValue),
            nullptr)));
      }
    }

    info_.params.push_back(std::move(param));
  }
  return true;
}

std::unique_ptr<FfglInstance> FfglLibrary::createInstance(uint32_t width,
                                                          uint32_t height,
                                                          std::string& error) {
  auto call = reinterpret_cast<PlugMainFn>(plugMain_);
  FFGLViewportStruct viewport = {0, 0, width, height};
  FFMixed result = call(FF_INSTANTIATE_GL, pointerMixed(&viewport), nullptr);
  if (result.UIntValue == FF_FAIL || result.PointerValue == nullptr) {
    error = "FF_INSTANTIATE_GL failed for " + info_.name;
    return nullptr;
  }
  return std::unique_ptr<FfglInstance>(
      new FfglInstance(this, result.PointerValue, width, height));
}

FfglInstance::~FfglInstance() {
  if (library_ && instanceId_) {
    reinterpret_cast<PlugMainFn>(library_->plugMain_)(
        FF_DEINSTANTIATE_GL, uintMixed(0), instanceId_);
  }
}

bool FfglInstance::setParamFloat(uint32_t index, float value) {
  SetParameterStruct set = {};
  set.ParameterNumber = index;
  set.NewParameterValue = floatToMixed(value);
  return reinterpret_cast<PlugMainFn>(library_->plugMain_)(
             FF_SET_PARAMETER, pointerMixed(&set), instanceId_)
             .UIntValue == FF_SUCCESS;
}

bool FfglInstance::setParamText(uint32_t index, const std::string& value) {
  SetParameterStruct set = {};
  set.ParameterNumber = index;
  // The plugin copies the string during the call; the cast away from const
  // matches the ABI, not any mutation.
  set.NewParameterValue = pointerMixed(const_cast<char*>(value.c_str()));
  return reinterpret_cast<PlugMainFn>(library_->plugMain_)(
             FF_SET_PARAMETER, pointerMixed(&set), instanceId_)
             .UIntValue == FF_SUCCESS;
}

bool FfglInstance::setTime(double seconds) {
  return reinterpret_cast<PlugMainFn>(library_->plugMain_)(
             FF_SET_TIME, pointerMixed(&seconds), instanceId_)
             .UIntValue == FF_SUCCESS;
}

bool FfglInstance::process(uint32_t inputTexture, uint32_t width,
                           uint32_t height, uint32_t hostFbo) {
  FFGLTextureStruct texture = {};
  texture.Width = width;
  texture.Height = height;
  texture.HardwareWidth = width;
  texture.HardwareHeight = height;
  texture.Handle = inputTexture;
  FFGLTextureStruct* textures[1] = {&texture};

  ProcessOpenGLStruct process = {};
  process.numInputTextures = inputTexture != 0 ? 1 : 0;
  process.inputTextures = inputTexture != 0 ? textures : nullptr;
  process.HostFBO = hostFbo;

  return reinterpret_cast<PlugMainFn>(library_->plugMain_)(
             FF_PROCESS_OPENGL, pointerMixed(&process), instanceId_)
             .UIntValue == FF_SUCCESS;
}

}  // namespace oxbow
