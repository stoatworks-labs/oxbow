#include "io/video_io.h"

#include "io/ndi.h"
#include "io/omt.h"

#if defined(__APPLE__)
#include "io/syphon.h"
#elif defined(_WIN32)
#include "io/spout.h"
#endif

#if defined(OXBOW_WITH_DECKLINK)
#include "io/decklink.h"
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
  std::string list = "ndi, omt, syphon";
#elif defined(_WIN32)
  std::string list = "ndi, omt, spout";
#else
  std::string list = "ndi, omt";
#endif
#if defined(OXBOW_WITH_DECKLINK)
  list += ", decklink";
#endif
  return list;
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
  // The shared-surface pair. Each is named for the protocol rather than for
  // "shared", because that is what the operator's other application calls it —
  // and answering "macOS only" beats "unknown protocol" for someone who copied
  // a command line from the other platform's documentation.
#if defined(__APPLE__)
  if (protocol == "syphon") return syphonCreateSender(name, error);
#elif defined(_WIN32)
  if (protocol == "spout") return spoutCreateSender(name, error);
#endif
  if (protocol == "syphon") {
    error = "syphon output is macOS only";
    return nullptr;
  }
  if (protocol == "spout") {
    error = "spout output is Windows only";
    return nullptr;
  }

#if defined(OXBOW_WITH_DECKLINK)
  if (protocol == "decklink") return decklinkCreateSender(name, error);
#else
  if (protocol == "decklink") {
    // Configured out, not missing: the SDK is Blackmagic's and cannot be
    // vendored, so this build simply never had it.
    error =
        "this build has no DeckLink support - configure with "
        "-DDECKLINK_SDK_DIR=/path/to/Blackmagic DeckLink SDK";
    return nullptr;
  }
#endif
  error = "unknown protocol \"" + protocol + "\" (" + sendProtocols() + ")";
  return nullptr;
}

}  // namespace oxbow
