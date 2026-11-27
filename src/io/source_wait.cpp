#include "io/source_wait.h"

#include <algorithm>
#include <cctype>

namespace oxbow {

std::vector<SourceInfo> waitForSources(const SourceWaitOps& ops,
                                       unsigned waitMs,
                                       const SourcesWanted& enough) {
  using Clock = std::chrono::steady_clock;
  auto now = [&ops] { return ops.now ? ops.now() : Clock::now(); };

  const Clock::time_point deadline =
      now() + std::chrono::milliseconds((long long)waitMs);
  for (;;) {
    // Rounded up, so the last wait reaches the deadline rather than stopping
    // short of it and spinning out the remainder.
    const Clock::time_point t = now();
    const long long left =
        t < deadline
            ? std::chrono::ceil<std::chrono::milliseconds>(deadline - t).count()
            : 0;
    if (ops.waitForChange((unsigned)left)) {
      std::vector<SourceInfo> sources = ops.current();
      if (enough && enough(sources)) return sources;
    }
    if (now() >= deadline) break;
  }
  return ops.current();
}

const SourceInfo* findSource(const std::vector<SourceInfo>& sources,
                             const std::string& nameSubstring) {
  auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
  };
  const std::string needle = lower(nameSubstring);
  for (const SourceInfo& source : sources)
    if (lower(source.name).find(needle) != std::string::npos) return &source;
  return nullptr;
}

}  // namespace oxbow
