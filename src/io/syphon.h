#pragma once

#include "io/video_io.h"

namespace oxbow {

/// A Syphon server, macOS only. Output-only: Syphon shares frames between
/// applications on one machine as an IOSurface, and oxbow has no reason to
/// receive one — anything that can publish Syphon can be pointed at NDI too,
/// and the receive side of this repo already speaks that.
///
/// No audio. Syphon is a video protocol; `sendAudio` is a no-op rather than an
/// error, because a chain configured with audio should not fail merely because
/// its video happens to be going out this way.
std::unique_ptr<VideoSender> syphonCreateSender(const std::string& name,
                                                std::string& error);

}  // namespace oxbow
