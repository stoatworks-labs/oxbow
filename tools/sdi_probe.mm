// sdi_probe — an independent SDI receiver for checking oxbow's DeckLink output.
//
// Captures on a DeckLink input, converts UYVY to RGB with a BT.709 transform
// written *here*, and checks the bars against values computed here too. Nothing
// in this file is shared with the output path, so a pass is two implementations
// agreeing rather than one agreeing with itself. It reports what came back off
// the wire, not what oxbow believes it sent.
//
// Build (macOS), pointing at the same SDK the main build uses:
//
//   clang++ -std=c++17 -fobjc-arc -o build/sdi_probe tools/sdi_probe.mm \
//     "$SDK/DeckLinkAPIDispatch.cpp" -I "$SDK" -framework CoreFoundation -w
//
//   ./build/sdi_probe --device 3 --out build/captured.ppm
//
// **The round trip is not byte-exact and must not be checked as though it
// were.** oxbow hands the card 8-bit BGRA; SDI carries 4:2:2 YUV, so the
// hardware converts on the way out and this converts back on the way in. Two
// conversions and chroma subsampling move values by a few counts, and any check
// tight enough to call that a failure is measuring the wire, not the code. The
// tolerance below is deliberately generous; what it is really asserting is that
// each bar is the *right colour in the right place*, which is what catches a
// channel swap, a flip, a stride error or a wrong mode.

#include "DeckLinkAPI.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <algorithm>
#include <vector>

namespace {

/// oxbow's send-test pattern: eight bars, the bar index's low three bits
/// driving R, G and B at 191. Repeated here from `runTestSender` deliberately —
/// if somebody changes the generator, this check should fail until they change
/// it here too.
struct Rgb { int r, g, b; };
Rgb expectedBar(int bar) {
  return {(bar & 1) ? 191 : 0, (bar & 2) ? 191 : 0, (bar & 4) ? 191 : 0};
}

/// Limited-range BT.709 YCbCr -> RGB. Written out rather than reused so this
/// file does not inherit a mistake from the code it is checking.
Rgb toRgb(int y, int cb, int cr) {
  const double yn = (y - 16) / 219.0;
  const double u = (cb - 128) / 224.0;
  const double v = (cr - 128) / 224.0;
  const double r = yn + 2.0 * (1.0 - 0.2126) * v;
  const double b = yn + 2.0 * (1.0 - 0.0722) * u;
  const double g = (yn - 0.2126 * r - 0.0722 * b) / 0.7152;
  auto clamp = [](double v01) {
    const int out = (int)std::lround(v01 * 255.0);
    return out < 0 ? 0 : (out > 255 ? 255 : out);
  };
  return {clamp(r), clamp(g), clamp(b)};
}

std::atomic<int> g_frames{0};
int g_width = 0, g_height = 0;
std::vector<uint8_t> g_frame;  // UYVY, one captured frame
int g_stride = 0;

class Capture : public IDeckLinkInputCallback {
 public:
  HRESULT QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return 1; }
  ULONG Release() override { return 1; }

  HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents,
                                  IDeckLinkDisplayMode* mode,
                                  BMDDetectedVideoInputFormatFlags) override {
    if (mode != nullptr) {
      std::printf("input format changed: %ldx%ld\n", (long)mode->GetWidth(),
                  (long)mode->GetHeight());
    }
    return S_OK;
  }

  HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame* frame,
                                 IDeckLinkAudioInputPacket*) override {
    if (frame == nullptr) return S_OK;
    // A frame with no source connected still arrives, flagged. Counting it
    // would report "signal present" for an unplugged cable.
    if ((frame->GetFlags() & bmdFrameHasNoInputSource) != 0) return S_OK;

    void* bytes = nullptr;
    if (frame->GetBytes(&bytes) != S_OK || bytes == nullptr) return S_OK;

    g_width = (int)frame->GetWidth();
    g_height = (int)frame->GetHeight();
    g_stride = (int)frame->GetRowBytes();
    // Keep the most recent frame whole; the checks run after capture stops so
    // nothing is decided on the callback thread.
    g_frame.assign((const uint8_t*)bytes, (const uint8_t*)bytes + (size_t)g_stride * g_height);
    ++g_frames;
    return S_OK;
  }
};

int sampleY(const uint8_t* row, int x) { return row[(x / 2) * 4 + ((x % 2) == 0 ? 1 : 3)]; }
int sampleCb(const uint8_t* row, int x) { return row[(x / 2) * 4 + 0]; }
int sampleCr(const uint8_t* row, int x) { return row[(x / 2) * 4 + 2]; }

}  // namespace

int main(int argc, char** argv) {
  int index = 3;
  std::string outPath;
  BMDDisplayMode mode = bmdModeHD720p60;
  int tolerance = 24;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (arg == "--device") { if (const char* v = next()) index = std::atoi(v); }
    else if (arg == "--out") { if (const char* v = next()) outPath = v; }
    else if (arg == "--tolerance") { if (const char* v = next()) tolerance = std::atoi(v); }
    else if (arg == "--mode") {
      const char* v = next();
      const std::string m = v ? v : "";
      if (m == "1080p50") mode = bmdModeHD1080p50;
      else if (m == "1080p30") mode = bmdModeHD1080p30;
      else if (m == "1080p25") mode = bmdModeHD1080p25;
      else if (m == "720p60") mode = bmdModeHD720p60;
      else if (m == "720p50") mode = bmdModeHD720p50;
      else { std::printf("unknown --mode %s\n", m.c_str()); return 2; }
    }
  }

  // `--list` answers the question that actually comes up on a multi-sub-device
  // card: which of these can capture *right now*. A Duo 2 lists all four
  // sub-devices whatever profile it is in, and one configured for output
  // refuses EnableVideoInput with no explanation — which reads as broken
  // hardware or a bad mode when it is neither.
  if (argc > 1 && std::string(argv[1]) == "--list") {
    IDeckLinkIterator* it = CreateDeckLinkIteratorInstance();
    if (it == nullptr) { std::printf("no DeckLink drivers\n"); return 2; }
    IDeckLink* d = nullptr;
    int n = 0;
    while (it->Next(&d) == S_OK) {
      CFStringRef nr = nullptr; d->GetDisplayName(&nr);
      char nb[256] = {};
      if (nr) { CFStringGetCString(nr, nb, sizeof(nb), kCFStringEncodingUTF8); CFRelease(nr); }

      IDeckLinkInput* in = nullptr;
      IDeckLinkOutput* out = nullptr;
      const bool hasIn = d->QueryInterface(IID_IDeckLinkInput, (void**)&in) == S_OK;
      const bool hasOut = d->QueryInterface(IID_IDeckLinkOutput, (void**)&out) == S_OK;

      int64_t duplex = -1;
      IDeckLinkProfileAttributes* attrs = nullptr;
      if (d->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attrs) == S_OK) {
        attrs->GetInt(BMDDeckLinkDuplex, &duplex);
        attrs->Release();
      }
      const char* duplexName = duplex == bmdDuplexFull ? "full"
                             : duplex == bmdDuplexHalf ? "half"
                             : duplex == bmdDuplexSimplex ? "simplex"
                             : duplex == bmdDuplexInactive ? "INACTIVE" : "?";

      int inputModes = 0;
      if (in != nullptr) {
        IDeckLinkDisplayModeIterator* mi = nullptr;
        if (in->GetDisplayModeIterator(&mi) == S_OK && mi != nullptr) {
          IDeckLinkDisplayMode* m = nullptr;
          while (mi->Next(&m) == S_OK) { ++inputModes; m->Release(); }
          mi->Release();
        }
      }
      std::printf("%d: %-22s input=%s output=%s duplex=%-8s inputModes=%d\n", n, nb,
                  hasIn ? "yes" : "no", hasOut ? "yes" : "no", duplexName, inputModes);
      if (in) in->Release();
      if (out) out->Release();
      d->Release();
      ++n;
    }
    it->Release();
    return 0;
  }

  IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
  if (iterator == nullptr) { std::printf("no DeckLink drivers\n"); return 2; }

  IDeckLink* device = nullptr;
  IDeckLink* chosen = nullptr;
  int i = 0;
  while (iterator->Next(&device) == S_OK) {
    if (i++ == index) { chosen = device; break; }
    device->Release();
  }
  iterator->Release();
  if (chosen == nullptr) { std::printf("no device at index %d\n", index); return 2; }

  CFStringRef nameRef = nullptr;
  chosen->GetDisplayName(&nameRef);
  char name[256] = {};
  if (nameRef != nullptr) {
    CFStringGetCString(nameRef, name, sizeof(name), kCFStringEncodingUTF8);
    CFRelease(nameRef);
  }
  std::printf("capturing on index %d: %s\n", index, name);

  IDeckLinkInput* input = nullptr;
  if (chosen->QueryInterface(IID_IDeckLinkInput, (void**)&input) != S_OK) {
    std::printf("device has no input interface\n");
    return 2;
  }

  Capture capture;
  input->SetCallback(&capture);
  if (input->EnableVideoInput(mode, bmdFormat8BitYUV, bmdVideoInputFlagDefault) != S_OK) {
    std::printf("EnableVideoInput failed\n");
    return 2;
  }
  if (input->StartStreams() != S_OK) { std::printf("StartStreams failed\n"); return 2; }

  for (int s = 0; s < 60 && g_frames < 8; ++s) usleep(100000);

  input->StopStreams();
  input->DisableVideoInput();
  input->SetCallback(nullptr);
  input->Release();
  chosen->Release();

  std::printf("frames received: %d", g_frames.load());
  if (g_width) std::printf("  (%dx%d, stride %d)", g_width, g_height, g_stride);
  std::printf("\n");

  if (g_frames == 0 || g_frame.empty()) {
    std::printf("NO SIGNAL on this input\n");
    return 1;
  }

  // ---- the check ----------------------------------------------------------
  //
  // Sample the middle row, and for each bar take the median across its width so
  // the moving white sweep — 40 px of a 160 px bar at 720p — cannot decide the
  // answer.
  const uint8_t* row = g_frame.data() + (size_t)(g_height / 2) * g_stride;
  int failures = 0;
  std::printf("\n%-5s %-16s %-16s %s\n", "bar", "expected", "captured", "");
  for (int bar = 0; bar < 8; ++bar) {
    const int x0 = bar * g_width / 8;
    const int x1 = (bar + 1) * g_width / 8;
    std::vector<int> rs, gs, bs;
    for (int x = x0 + 4; x < x1 - 4; x += 2) {
      const Rgb rgb = toRgb(sampleY(row, x), sampleCb(row, x), sampleCr(row, x));
      rs.push_back(rgb.r); gs.push_back(rgb.g); bs.push_back(rgb.b);
    }
    auto median = [](std::vector<int>& v) {
      std::sort(v.begin(), v.end());
      return v.empty() ? 0 : v[v.size() / 2];
    };
    const Rgb got{median(rs), median(gs), median(bs)};
    const Rgb want = expectedBar(bar);
    const int dr = std::abs(got.r - want.r), dg = std::abs(got.g - want.g),
              db = std::abs(got.b - want.b);
    const bool ok = dr <= tolerance && dg <= tolerance && db <= tolerance;
    if (!ok) ++failures;
    std::printf("%-5d %3d,%3d,%3d      %3d,%3d,%3d      %s\n", bar, want.r, want.g, want.b,
                got.r, got.g, got.b, ok ? "ok" : "MISMATCH");
  }

  if (!outPath.empty()) {
    FILE* file = std::fopen(outPath.c_str(), "wb");
    if (file != nullptr) {
      std::fprintf(file, "P6\n%d %d\n255\n", g_width, g_height);
      for (int y = 0; y < g_height; ++y) {
        const uint8_t* r = g_frame.data() + (size_t)y * g_stride;
        for (int x = 0; x < g_width; ++x) {
          const Rgb rgb = toRgb(sampleY(r, x), sampleCb(r, x), sampleCr(r, x));
          const uint8_t px[3] = {(uint8_t)rgb.r, (uint8_t)rgb.g, (uint8_t)rgb.b};
          std::fwrite(px, 1, 3, file);
        }
      }
      std::fclose(file);
      std::printf("\nwrote %s\n", outPath.c_str());
    }
  }

  std::printf("%s (tolerance +/-%d, a 4:2:2 round trip is not byte-exact)\n",
              failures == 0 ? "PASS" : "FAIL", tolerance);
  return failures == 0 ? 0 : 1;
}
