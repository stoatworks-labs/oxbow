// waitForSources and findSource, driven by a scripted discovery list on a fake
// clock — no NDI runtime, no network.
//
// The fake behaves as NDIlib_find_wait_for_sources does: a wait returns as
// soon as the list changes, not when its timeout runs out. The first check
// shows one wait alone missing a sender announced after a local one, which is
// the bug: on a Mac with "MAC (macOS AV Output)" running, `oxbow list --proto
// ndi` never listed a Windows VM's sender on the same LAN, and a build that
// waited out the window did.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "io/source_wait.h"

using namespace oxbow;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      ++g_failures;                                                    \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                  \
  } while (0)

const SourceInfo kLocal{"MAC (macOS AV Output)", "192.168.1.90:5961"};
const SourceInfo kRemote{"WIN-LAB (FFWIN NDI)", "192.168.1.143:5961"};
const SourceInfo kOther{"STUDIO-PC (vMix - Output 1)", "192.168.1.20:5961"};

bool lists(const std::vector<SourceInfo>& sources, const SourceInfo& wanted) {
  for (const SourceInfo& source : sources)
    if (source.name == wanted.name && source.url == wanted.url) return true;
  return false;
}

// A discovery list scripted in time, on a clock that moves only when the code
// under test waits.
class FakeDiscovery {
 public:
  struct Change {
    unsigned atMs;
    std::vector<SourceInfo> list;
  };

  explicit FakeDiscovery(std::vector<Change> changes)
      : changes_(std::move(changes)) {}

  SourceWaitOps ops() {
    SourceWaitOps ops;
    ops.waitForChange = [this](unsigned ms) { return wait(ms); };
    ops.current = [this] {
      ++reads;
      return list_;
    };
    ops.now = [this] { return epoch_ + std::chrono::milliseconds(nowMs); };
    return ops;
  }

  unsigned nowMs = 0;
  int waits = 0;
  int reads = 0;

 private:
  bool wait(unsigned ms) {
    ++waits;
    const unsigned until = nowMs + ms;
    if (next_ < changes_.size() && changes_[next_].atMs <= until) {
      if (changes_[next_].atMs > nowMs) nowMs = changes_[next_].atMs;
      list_ = changes_[next_].list;
      ++next_;
      return true;
    }
    nowMs = until;
    return false;
  }

  // Any fixed point will do; steady_clock's epoch is unspecified.
  const std::chrono::steady_clock::time_point epoch_ =
      std::chrono::steady_clock::time_point(std::chrono::hours(1));
  std::vector<Change> changes_;
  size_t next_ = 0;
  std::vector<SourceInfo> list_;
};

SourcesWanted named(const std::string& nameSubstring) {
  return [nameSubstring](const std::vector<SourceInfo>& seen) {
    return findSource(seen, nameSubstring) != nullptr;
  };
}

void oneWaitAloneStopsAtTheFirstChange() {
  FakeDiscovery fake({{50, {kLocal}}, {1200, {kLocal, kRemote}}});
  SourceWaitOps ops = fake.ops();
  CHECK(ops.waitForChange(3000));
  std::vector<SourceInfo> seen = ops.current();
  CHECK(fake.nowMs == 50);
  CHECK(seen.size() == 1);
  CHECK(!lists(seen, kRemote));
}

void aRemoteSourceAnnouncedAfterALocalOneIsListed() {
  FakeDiscovery fake({{50, {kLocal}}, {1200, {kLocal, kRemote}}});
  std::vector<SourceInfo> seen = waitForSources(fake.ops(), 3000);
  CHECK(seen.size() == 2);
  CHECK(lists(seen, kLocal));
  CHECK(lists(seen, kRemote));
  CHECK(fake.nowMs == 3000);
}

void listingWaitsTheWholeWindowAndNoLonger() {
  FakeDiscovery fake({});
  std::vector<SourceInfo> seen = waitForSources(fake.ops(), 3000);
  CHECK(seen.empty());
  CHECK(fake.nowMs == 3000);
}

void aSourceAnnouncedAfterTheWindowIsNotWaitedFor() {
  FakeDiscovery fake({{50, {kLocal}}, {4000, {kLocal, kRemote}}});
  std::vector<SourceInfo> seen = waitForSources(fake.ops(), 3000);
  CHECK(seen.size() == 1);
  CHECK(lists(seen, kLocal));
  CHECK(fake.nowMs == 3000);
}

void aSourceThatGoesAwayIsNotListed() {
  FakeDiscovery fake({{50, {kLocal, kRemote}}, {800, {kLocal}}});
  std::vector<SourceInfo> seen = waitForSources(fake.ops(), 3000);
  CHECK(seen.size() == 1);
  CHECK(lists(seen, kLocal));
}

void theListIsReadAfterEveryChange() {
  // As the SDK's own find example does: wait, and on a change read the list.
  FakeDiscovery fake({{50, {kLocal}},
                      {1200, {kLocal, kRemote}},
                      {2000, {kLocal, kRemote, kOther}}});
  std::vector<SourceInfo> seen = waitForSources(fake.ops(), 3000);
  CHECK(seen.size() == 3);
  CHECK(fake.reads == 4);// three changes, then the list at the deadline
}

void theReceiverStopsAtItsSource() {
  FakeDiscovery fake({{50, {kLocal}},
                      {1200, {kLocal, kRemote}},
                      {2000, {kLocal, kRemote, kOther}}});
  std::vector<SourceInfo> seen =
      waitForSources(fake.ops(), 5000, named("ffwin"));
  CHECK(fake.nowMs == 1200);
  CHECK(lists(seen, kRemote));
  CHECK(findSource(seen, "ffwin") != nullptr);
}

void aLocalSourceConnectsAtOnce() {
  FakeDiscovery fake({{50, {kLocal}}, {1200, {kLocal, kRemote}}});
  std::vector<SourceInfo> seen =
      waitForSources(fake.ops(), 5000, named("macOS AV"));
  CHECK(fake.nowMs == 50);
  CHECK(seen.size() == 1);
}

void aReceiverWithNoMatchWaitsTheWholeWindow() {
  FakeDiscovery fake({{50, {kLocal}}, {1200, {kLocal, kRemote}}});
  std::vector<SourceInfo> seen =
      waitForSources(fake.ops(), 5000, named("not there"));
  CHECK(fake.nowMs == 5000);
  CHECK(seen.size() == 2);// so "saw N sources" counts what was really seen
}

void aZeroWaitStillReadsTheList() {
  FakeDiscovery present({{0, {kLocal}}});
  std::vector<SourceInfo> seen = waitForSources(present.ops(), 0);
  CHECK(seen.size() == 1);
  CHECK(present.waits == 1);
  CHECK(present.nowMs == 0);

  FakeDiscovery empty({});
  CHECK(waitForSources(empty.ops(), 0).empty());
  CHECK(empty.waits == 1);
}

void findSourceIgnoresCaseAndTakesTheFirst() {
  const std::vector<SourceInfo> sources = {kLocal, kRemote};
  CHECK(findSource(sources, "ffwin") == &sources[1]);
  CHECK(findSource(sources, "WIN-LAB (FFWIN NDI)") == &sources[1]);
  CHECK(findSource(sources, "win-lab") == &sources[1]);
  CHECK(findSource(sources, "MAC") == &sources[0]);
  CHECK(findSource(sources, "nope") == nullptr);
  CHECK(findSource({}, "ffwin") == nullptr);
}

void theDefaultClockIsTheRealOne() {
  // No `now` given: the deadline is on steady_clock, so a wait that never
  // sees a change takes the whole window in real time.
  SourceWaitOps ops;
  ops.waitForChange = [](unsigned ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return false;
  };
  ops.current = [] { return std::vector<SourceInfo>{kLocal}; };
  const auto start = std::chrono::steady_clock::now();
  std::vector<SourceInfo> seen = waitForSources(ops, 40);
  const auto took = std::chrono::steady_clock::now() - start;
  CHECK(seen.size() == 1);
  CHECK(took >= std::chrono::milliseconds(40));
}

}  // namespace

int main() {
  struct Test {
    const char* name;
    void (*run)();
  };
  const Test tests[] = {
      {"oneWaitAloneStopsAtTheFirstChange", oneWaitAloneStopsAtTheFirstChange},
      {"aRemoteSourceAnnouncedAfterALocalOneIsListed",
       aRemoteSourceAnnouncedAfterALocalOneIsListed},
      {"listingWaitsTheWholeWindowAndNoLonger",
       listingWaitsTheWholeWindowAndNoLonger},
      {"aSourceAnnouncedAfterTheWindowIsNotWaitedFor",
       aSourceAnnouncedAfterTheWindowIsNotWaitedFor},
      {"aSourceThatGoesAwayIsNotListed", aSourceThatGoesAwayIsNotListed},
      {"theListIsReadAfterEveryChange", theListIsReadAfterEveryChange},
      {"theReceiverStopsAtItsSource", theReceiverStopsAtItsSource},
      {"aLocalSourceConnectsAtOnce", aLocalSourceConnectsAtOnce},
      {"aReceiverWithNoMatchWaitsTheWholeWindow",
       aReceiverWithNoMatchWaitsTheWholeWindow},
      {"aZeroWaitStillReadsTheList", aZeroWaitStillReadsTheList},
      {"findSourceIgnoresCaseAndTakesTheFirst",
       findSourceIgnoresCaseAndTakesTheFirst},
      {"theDefaultClockIsTheRealOne", theDefaultClockIsTheRealOne},
  };
  for (const Test& test : tests) {
    const int before = g_failures;
    test.run();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", test.name);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
