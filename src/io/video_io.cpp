#include "io/video_io.h"

#include "io/ndi.h"
#include "io/omt.h"

#if defined(__APPLE__)
#include "io/syphon.h"
#endif

namespace oxbow {
namespace {

/// The protocols this build can *receive* on, for error messages. Syphon is
/// deliberately absent: it is an output here, and saying "ndi or omt" when the
/// operator typed `--in-proto syphon` is more useful than listing a protocol
/// that will then fail for a different reason.
constexpr const char* kReceiveProtocols = "ndi or omt";

std::string sendProtocols() {
#if defined(__APPLE__)
  return "ndi, omt or syphon";
#else
  return "ndi or omt";
#endif
}

}  // namespace

std::vector<SourceInfo> listSources(const std::string& protocol,
                                    unsigned waitMs, std::string& error) {
  if (protocol == "ndi") return ndiListSources(waitMs, error);
  if (protocol == "omt") return omtListSources(waitMs, error);
  error = "unknown protocol \"" + protocol + "\" (" + kReceiveProtocols + ")";
  return {};
}

std::unique_ptr<VideoReceiver> connectReceiver(const std::string& protocol,
                                               const std::string& nameSubstring,
                                               unsigned waitMs,
                                               std::string& error) {
  if (protocol == "ndi") return ndiConnectReceiver(nameSubstring, waitMs, error);
  if (protocol == "omt") return omtConnectReceiver(nameSubstring, waitMs, error);
  error = "unknown protocol \"" + protocol + "\" (" + kReceiveProtocols + ")";
  return nullptr;
}

std::unique_ptr<VideoSender> createSender(const std::string& protocol,
                                          const std::string& name,
                                          std::string& error) {
  if (protocol == "ndi") return ndiCreateSender(name, error);
  if (protocol == "omt") return omtCreateSender(name, error);
#if defined(__APPLE__)
  if (protocol == "syphon") return syphonCreateSender(name, error);
#else
  if (protocol == "syphon") {
    error = "syphon output is macOS only";
    return nullptr;
  }
#endif
  error = "unknown protocol \"" + protocol + "\" (" + sendProtocols() + ")";
  return nullptr;
}

}  // namespace oxbow
