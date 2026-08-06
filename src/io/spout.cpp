// Spout sender, Windows.
//
// Spout shares a frame between applications as a DirectX 11 texture with a
// DXGI shared handle, named through shared memory so a receiver can find it by
// name. The SDK's `spoutDX` class is the DirectX-only half — no OpenGL, no
// window, no message pump — which makes this the direct counterpart of the
// Syphon backend next door:
//
//     OpenDirectX11(nullptr)     spoutDX creates its own D3D11 device
//     SetSenderName(name)
//     SendImage(pixels, w, h, pitch)
//
// `SendImage` calls `UpdateSubresource` straight onto the shared texture with
// the pitch it is handed, and spoutDX's default sender format is
// DXGI_FORMAT_B8G8R8A8_UNORM — exactly the BGRA oxbow's pump already has. No
// conversion, no flip, no intermediate buffer, and there should not be one.
//
// Ported from WebLinked's `src/outputs/shared_surface_win.cpp`.
//
// **This file has never been run.** It is written against the Windows SDK and
// the vendored Spout sources and nothing more. It does compile: this file and
// all seven vendored Spout sources built in the Windows job of run 31129182404
// (2026-08-06). Compiling is not evidence that a receiver sees a picture, and
// this comment should not be deleted until somebody has looked at one.
//
// Two deliberate differences from Syphon, both properties of the protocol:
//
//   - Spout gives a *sender* no way to learn whether anyone is receiving
//     (`spoutDX::IsConnected` is the receiver's question), so there is no
//     skip-when-nobody-is-listening optimisation and every frame pays for the
//     copy. Reporting nothing beats inventing an attach signal.
//   - The sender is named before the first send. `SendImage` would otherwise
//     create it under spoutDX's own default name, and an operator looking for
//     the name they typed would not find it.
//
// There is no main-thread requirement here — that is a Syphon/
// NSDistributedNotificationCenter constraint, not a shared-surface one.

#include "io/spout.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

#include "SpoutDX.h"

namespace oxbow {
namespace {

class SpoutSender final : public VideoSender {
 public:
  explicit SpoutSender(std::string name) : name_(std::move(name)) {}

  ~SpoutSender() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return;
    open_ = false;
    // Releasing retires the name, so a receiver's source list loses the entry
    // now rather than keeping a dead one until it times out.
    sender_.ReleaseSender();
    sender_.CloseDirectX11();
  }

  void sendVideo(const uint8_t* data, int width, int height, int /*frameRateN*/,
                 int /*frameRateD*/, int64_t /*timestamp*/) override {
    if (data == nullptr || width <= 0 || height <= 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureOpen()) return;

    // oxbow hands every sender tightly packed BGRA, so the pitch is exactly
    // the row.
    sender_.SendImage(data, static_cast<unsigned int>(width),
                      static_cast<unsigned int>(height),
                      static_cast<unsigned int>(width) * 4);
  }

  /// Spout carries video only.
  void sendAudio(const AudioFrame&) override {}

 private:
  /// Deferred to the first frame, like Syphon's: oxbow's senders learn the
  /// raster from the frame rather than from an open() call. Spout does not need
  /// the size up front, but keeping the two backends the same shape means one
  /// description of when a sender starts existing.
  bool ensureOpen() {
    if (open_) return true;
    if (failed_) return false;

    // A null device asks spoutDX to create its own.
    if (!sender_.OpenDirectX11(nullptr)) {
      failed_ = true;
      std::fprintf(stderr, "spout: could not open a DirectX 11 device\n");
      return false;
    }
    if (!sender_.SetSenderName(name_.c_str())) {
      sender_.CloseDirectX11();
      failed_ = true;
      std::fprintf(stderr, "spout: could not create a sender named \"%s\"\n",
                   name_.c_str());
      return false;
    }

    open_ = true;
    std::fprintf(stderr, "spout: publishing \"%s\"\n", name_.c_str());
    return true;
  }

  std::string name_;
  std::mutex mutex_;
  spoutDX sender_;
  bool open_ = false;
  bool failed_ = false;
};

}  // namespace

std::unique_ptr<VideoSender> spoutCreateSender(const std::string& name,
                                               std::string& error) {
  if (name.empty()) {
    error = "a Spout sender needs a name";
    return nullptr;
  }
  return std::make_unique<SpoutSender>(name);
}

}  // namespace oxbow
