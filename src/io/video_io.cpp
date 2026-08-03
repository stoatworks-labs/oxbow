#include "io/video_io.h"

#include "io/ndi.h"
#include "io/omt.h"

namespace oxbow {

std::vector<SourceInfo> listSources(const std::string& protocol,
                                    unsigned waitMs, std::string& error) {
  if (protocol == "ndi") return ndiListSources(waitMs, error);
  if (protocol == "omt") return omtListSources(waitMs, error);
  error = "unknown protocol \"" + protocol + "\" (ndi or omt)";
  return {};
}

std::unique_ptr<VideoReceiver> connectReceiver(const std::string& protocol,
                                               const std::string& nameSubstring,
                                               unsigned waitMs,
                                               std::string& error) {
  if (protocol == "ndi") return ndiConnectReceiver(nameSubstring, waitMs, error);
  if (protocol == "omt") return omtConnectReceiver(nameSubstring, waitMs, error);
  error = "unknown protocol \"" + protocol + "\" (ndi or omt)";
  return nullptr;
}

std::unique_ptr<VideoSender> createSender(const std::string& protocol,
                                          const std::string& name,
                                          std::string& error) {
  if (protocol == "ndi") return ndiCreateSender(name, error);
  if (protocol == "omt") return omtCreateSender(name, error);
  error = "unknown protocol \"" + protocol + "\" (ndi or omt)";
  return nullptr;
}

}  // namespace oxbow
