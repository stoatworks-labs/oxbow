#pragma once

#include "io/video_io.h"

namespace oxbow {

/// A Spout sender, Windows only — the counterpart of the Syphon output on
/// macOS. Output-only, and no audio, for the same reasons.
std::unique_ptr<VideoSender> spoutCreateSender(const std::string& name,
                                               std::string& error);

}  // namespace oxbow
