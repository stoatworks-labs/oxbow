#pragma once

#include "io/video_io.h"

namespace oxbow {

std::vector<SourceInfo> omtListSources(unsigned waitMs, std::string& error);
std::unique_ptr<VideoReceiver> omtConnectReceiver(
    const std::string& nameSubstring, unsigned waitMs, std::string& error);
std::unique_ptr<VideoSender> omtCreateSender(const std::string& name,
                                             std::string& error);

}  // namespace oxbow
