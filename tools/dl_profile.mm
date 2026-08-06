// dl_profile — read, and optionally change, a DeckLink card's profile.
//
//   ./build/dl_profile                 # list profiles, mark the active one
//   ./build/dl_profile --set 4dhd      # activate four sub-devices, half duplex
//
// **This writes to the card and the change persists.** It affects every
// application using it, not just oxbow, which is why it lives in its own tool
// with a name that says so rather than inside the read-only probe.
//
// Why it exists: a Duo 2 lists all four sub-devices whatever profile it is in,
// and a sub-device the profile has switched off reports `duplex=INACTIVE`,
// refuses EnableVideoInput and EnableVideoOutput, and offers a full list of
// display modes while doing it. That reads as broken hardware or a bad mode. It
// is neither — it is the profile. Measured on this card: with sub-device 2 in
// full duplex, connector 4 was unusable for both directions, so a cable between
// connectors 1 and 4 could not carry a loopback test at all.

#include "DeckLinkAPI.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

const char* profileName(int64_t id) {
  switch (id) {
    case bmdProfileOneSubDeviceFullDuplex: return "1 sub-device, full duplex   (1dfd)";
    case bmdProfileOneSubDeviceHalfDuplex: return "1 sub-device, half duplex   (1dhd)";
    case bmdProfileTwoSubDevicesFullDuplex: return "2 sub-devices, full duplex  (2dfd)";
    case bmdProfileTwoSubDevicesHalfDuplex: return "2 sub-devices, half duplex  (2dhd)";
    case bmdProfileFourSubDevicesHalfDuplex: return "4 sub-devices, half duplex  (4dhd)";
    default: return "unknown";
  }
}

int64_t profileFromArg(const std::string& arg) {
  if (arg == "1dfd") return bmdProfileOneSubDeviceFullDuplex;
  if (arg == "1dhd") return bmdProfileOneSubDeviceHalfDuplex;
  if (arg == "2dfd") return bmdProfileTwoSubDevicesFullDuplex;
  if (arg == "2dhd") return bmdProfileTwoSubDevicesHalfDuplex;
  if (arg == "4dhd") return bmdProfileFourSubDevicesHalfDuplex;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  int64_t wanted = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
      wanted = profileFromArg(argv[++i]);
      if (wanted == 0) {
        std::printf("unknown profile; use 1dfd, 1dhd, 2dfd, 2dhd or 4dhd\n");
        return 2;
      }
    }
  }

  IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
  if (iterator == nullptr) { std::printf("no DeckLink drivers\n"); return 2; }

  IDeckLink* device = nullptr;
  bool handled = false;
  int index = 0;
  while (iterator->Next(&device) == S_OK) {
    // The profile manager lives on the card, and only the sub-devices that can
    // reach it answer this query — so this walks until one does rather than
    // assuming it is the first.
    IDeckLinkProfileManager* manager = nullptr;
    if (device->QueryInterface(IID_IDeckLinkProfileManager, (void**)&manager) == S_OK &&
        manager != nullptr) {
      CFStringRef nameRef = nullptr;
      device->GetDisplayName(&nameRef);
      char name[256] = {};
      if (nameRef != nullptr) {
        CFStringGetCString(nameRef, name, sizeof(name), kCFStringEncodingUTF8);
        CFRelease(nameRef);
      }
      std::printf("card reached via index %d (%s)\n\n", index, name);

      IDeckLinkProfileIterator* profiles = nullptr;
      if (manager->GetProfiles(&profiles) == S_OK && profiles != nullptr) {
        IDeckLinkProfile* profile = nullptr;
        while (profiles->Next(&profile) == S_OK) {
          IDeckLinkProfileAttributes* attributes = nullptr;
          int64_t id = 0;
          if (profile->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attributes) ==
              S_OK) {
            attributes->GetInt(BMDDeckLinkProfileID, &id);
            attributes->Release();
          }
          bool active = false;
          profile->IsActive(&active);
          std::printf("  %s %s\n", active ? "*" : " ", profileName(id));

          if (wanted != 0 && id == wanted && !active) {
            std::printf("\nactivating %s ...\n", profileName(id));
            // Sub-devices are removed and re-added around this, so anything
            // holding one must be closed first — and re-enumerated after.
            const HRESULT result = profile->SetActive();
            std::printf("SetActive: %s\n", result == S_OK ? "ok" : "FAILED");
          } else if (wanted != 0 && id == wanted && active) {
            std::printf("\nalready active, nothing to do\n");
          }
          profile->Release();
        }
        profiles->Release();
      }
      manager->Release();
      handled = true;
      std::printf("\n");
    }
    device->Release();
    ++index;
  }
  iterator->Release();

  if (!handled) {
    std::printf("no sub-device exposed a profile manager — this card may not have profiles\n");
    return 1;
  }
  return 0;
}
