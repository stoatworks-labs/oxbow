// Syphon server, macOS.
//
// Syphon shares a frame between applications as an IOSurface — a buffer both
// the CPU and the GPU can address, and which a mach port hands to another
// process without a copy. The vendored framework's own servers exist to get a
// GL or Metal texture *into* such a surface; oxbow's pump has already read the
// chain's output back to CPU BGRA for the network transports, so this backend
// skips both renderers and writes the surface directly:
//
//     newSurfaceForWidth:height:options:   (SyphonSubclassing, BGRA8)
//     IOSurfaceLock -> copy rows -> IOSurfaceUnlock
//     publish                              (SyphonSubclassing)
//
// Ported from WebLinked's `src/outputs/shared_surface_mac.mm`, which is
// verified against Resolume Arena's bundled Syphon 5 client. One thing did NOT
// port, and it is the whole reason this file has a thread in it — see below.
//
// ## Why the server is created on the main thread
//
// `SyphonServerBase` registers for the announce-request notification from
// `-init`, and NSDistributedNotificationCenter delivers those on the **main**
// run loop. A private run-loop thread is not good enough, and this was measured
// rather than assumed: created on its own CFRunLoop thread the server is
// well-formed — right name, right UUID, SyphonSurfaceTypeIOSurface — announces
// itself, and is then invisible to every consumer. WebLinked's identical code
// on the main thread is found immediately by the same probe.
//
// A server nobody can discover is the worst shape this failure could take: it
// answers its opening announce, so a consumer already running finds it, and one
// started afterwards never does. That survives every short test and appears on
// the night.
//
// oxbow has no run loop of its own — the pump owns a frame thread and the main
// thread waits — so `app/main_loop.h` makes every main-thread wait service the
// run loop instead of sleeping. Publishing does not need the main thread;
// WebLinked publishes from its clock thread against a main-thread server, and
// that is the path verified against Resolume.

#include "io/syphon.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Syphon/SyphonServerBase.h>
#import <Syphon/SyphonSubclassing.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

/// The two SyphonSubclassing hooks this backend needs, and nothing else.
/// Declared as a subclass rather than calling the category on a bare
/// SyphonServerBase so that "which parts of Syphon we actually use" is one
/// short interface instead of a grep.
@interface OxbowSyphonServer : SyphonServerBase
/// A BGRA8 IOSurface of this size. Retained; the caller CFReleases it. Syphon
/// caches one internally and returns the same surface while the dimensions are
/// unchanged.
- (IOSurfaceRef)newSurfaceForWidth:(size_t)width height:(size_t)height;
/// Announces that the surface holds a new frame.
- (void)publishFrame;
@end

@implementation OxbowSyphonServer

- (IOSurfaceRef)newSurfaceForWidth:(size_t)width height:(size_t)height {
  return [self newSurfaceForWidth:width height:height options:nil];
}

- (void)publishFrame {
  [self publish];
}

@end

namespace oxbow {
namespace {

/// Runs `block` on the main thread and waits for it.
///
/// The isMainThread test is not an optimisation — `dispatch_sync` to the main
/// queue *from* the main thread deadlocks outright, and `send-test` calls
/// sendVideo from the main thread while `run` calls it from the pump's frame
/// thread, so both cases are real.
///
/// This requires the main thread to be inside a run loop; see
/// `app/main_loop.h` for why every main-thread wait in oxbow goes through
/// `waitServicingMainLoop`.
void runOnMain(void (^block)(void)) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

class SyphonSender final : public VideoSender {
 public:
  explicit SyphonSender(std::string name) : name_(std::move(name)) {}

  ~SyphonSender() override {
    // Detach under the lock, tear down outside it: teardown marshals to the
    // run-loop thread, and holding the lock across that would let a busy run
    // loop block a frame thread inside sendVideo().
    OxbowSyphonServer* server = nil;
    IOSurfaceRef surface = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::swap(server, server_);
      std::swap(surface, surface_);
    }
    if (surface != nullptr) CFRelease(surface);
    if (server != nil) {
      runOnMain(^{
        [server stop];
      });
    }
  }

  /// oxbow's transports learn the frame size from the first frame rather than
  /// from an open() call, so the server is created here and the surface is
  /// rebuilt whenever the raster changes.
  void sendVideo(const uint8_t* data, int width, int height, int /*frameRateN*/,
                 int /*frameRateD*/, int64_t /*timestamp*/) override {
    if (data == nullptr || width <= 0 || height <= 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureSurface(width, height)) return;

    // Syphon's own advice, and the reason skipped_ exists: with nobody
    // attached there is no point copying 8 MB fifty times a second.
    if (![server_ hasClients]) {
      ++skipped_;
      return;
    }

    if (IOSurfaceLock(surface_, 0, nullptr) != kIOReturnSuccess) return;

    auto* destination = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface_));
    const size_t destinationStride = IOSurfaceGetBytesPerRow(surface_);
    const size_t sourceStride = static_cast<size_t>(width) * 4;
    // IOSurface pads its rows to its own alignment, which is not the frame's,
    // so this is row by row rather than one memcpy.
    const size_t rowBytes = std::min(sourceStride, destinationStride);
    for (int y = 0; y < height; ++y) {
      std::memcpy(destination + (static_cast<size_t>(y) * destinationStride),
                  data + (static_cast<size_t>(y) * sourceStride), rowBytes);
    }
    IOSurfaceUnlock(surface_, 0, nullptr);

    [server_ publishFrame];
    ++published_;
  }

  /// Syphon carries video only. Silently ignored so that a chain with audio
  /// configured still runs; the CLI says so when this output is selected.
  void sendAudio(const AudioFrame&) override {}

 private:
  bool ensureSurface(int width, int height) {
    if (server_ != nil && surface_ != nullptr && width == width_ && height == height_) {
      return true;
    }

    if (surface_ != nullptr) {
      CFRelease(surface_);
      surface_ = nullptr;
    }

    __block OxbowSyphonServer* server = server_;
    __block IOSurfaceRef surface = nullptr;
    NSString* serverName = [NSString stringWithUTF8String:name_.c_str()];
    const size_t surfaceWidth = static_cast<size_t>(width);
    const size_t surfaceHeight = static_cast<size_t>(height);

    runOnMain(^{
      if (server == nil) {
        server = [[OxbowSyphonServer alloc] initWithName:serverName options:nil];
      }
      if (server != nil) {
        surface = [server newSurfaceForWidth:surfaceWidth height:surfaceHeight];
      }
    });

    // One line, on stderr, the first time only. This is the moment that can
    // fail invisibly — a Syphon server that exists but cannot be discovered
    // looks exactly like one that was never created — so it is reported even
    // when it works, and it is the line to ask an operator for.
    if (!reported_) {
      reported_ = true;
      if (server != nil && surface != nullptr) {
        std::fprintf(stderr, "syphon: publishing \"%s\" %dx%d\n", name_.c_str(), width,
                     height);
      } else {
        std::fprintf(stderr, "syphon: could not start server \"%s\" (%s)\n", name_.c_str(),
                     server == nil ? "server" : "surface");
      }
    }

    if (server == nil || surface == nullptr) return false;

    server_ = server;
    surface_ = surface;
    width_ = width;
    height_ = height;
    return true;
  }

  std::string name_;
  std::mutex mutex_;
  OxbowSyphonServer* server_ = nil;
  IOSurfaceRef surface_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int64_t published_ = 0;
  int64_t skipped_ = 0;
  bool reported_ = false;
};

}  // namespace

std::unique_ptr<VideoSender> syphonCreateSender(const std::string& name,
                                                std::string& error) {
  if (name.empty()) {
    error = "a Syphon server needs a name";
    return nullptr;
  }
  // Creation cannot fail here: the server is made when the first frame arrives
  // and its size is known. A name clash is not an error — Syphon disambiguates
  // by process.
  return std::make_unique<SyphonSender>(name);
}

}  // namespace oxbow
