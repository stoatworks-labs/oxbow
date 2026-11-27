#pragma once

// Waiting on a discovery list that fills in one source at a time.
//
// NDIlib_find_wait_for_sources returns as soon as the list CHANGES, not when
// its timeout runs out. Senders on this machine are announced first, so one
// call returns holding only those, and a sender on another machine is missed
// whenever any local one exists: `oxbow list --proto ndi` showed only local
// sources, and `--in "<remote name>"` failed with "no NDI source matching".
// The wait has to be repeated with whatever time is left.
//
// SDK-free, so a test can drive it with a fake wait and a fake clock.

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "io/video_io.h"

namespace oxbow {

struct SourceWaitOps {
  /// Blocks for up to `ms` until the list changes; true if it did.
  std::function<bool(unsigned ms)> waitForChange;
  /// The list as it stands right now.
  std::function<std::vector<SourceInfo>()> current;
  /// Left empty, std::chrono::steady_clock::now.
  std::function<std::chrono::steady_clock::time_point()> now;
};

using SourcesWanted = std::function<bool(const std::vector<SourceInfo>&)>;

/// Waits the whole of `waitMs` and returns the list as it stands at the end.
/// If `enough` is given, returns early with the first list it accepts after a
/// change. The list is read after every change, as the NDI SDK's own find
/// example does. Setup only: nothing here runs per frame.
std::vector<SourceInfo> waitForSources(const SourceWaitOps& ops,
                                       unsigned waitMs,
                                       const SourcesWanted& enough = {});

/// The first source whose name contains `nameSubstring`, ignoring case, or
/// nullptr.
const SourceInfo* findSource(const std::vector<SourceInfo>& sources,
                             const std::string& nameSubstring);

}  // namespace oxbow
