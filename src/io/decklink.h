#pragma once

#include "io/video_io.h"

namespace oxbow {

/// A DeckLink SDI/HDMI output. Compiled only when the build was configured with
/// `-DDECKLINK_SDK_DIR=...`; without it `createSender("decklink", …)` explains
/// that rather than failing obscurely.
///
/// `name` selects the device: a decimal index ("0", "1") or a substring of the
/// device's display name ("Duo", "UltraStudio"). An empty name takes the first
/// device, which is right for the common case of exactly one card.
///
/// No audio. oxbow passes audio through from its receiver, and putting it on
/// the card means scheduling it on the same timeline as the video, which is a
/// second thing to get wrong and cannot be tested without hardware. `sendAudio`
/// is a documented no-op rather than a half-done stream.
std::unique_ptr<VideoSender> decklinkCreateSender(const std::string& name,
                                                  std::string& error);

}  // namespace oxbow
