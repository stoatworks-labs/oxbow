// `oxbow run`: one pump plus, when configured, the control server.

#include <chrono>
#include <cstdio>
#include <thread>

#include "app/pump.h"
#include "control/control_api.h"

namespace oxbow {

int runPump(const PumpOptions& options) {
  Pump pump(options);
  std::string error;
  if (!pump.start(error)) {
    std::fprintf(stderr, "pump: %s\n", error.c_str());
    return 1;
  }

  ControlApi control;
  if (options.controlPort > 0) {
    if (!control.start(pump, options.controlBind, options.controlPort,
                       error)) {
      std::fprintf(stderr, "control: %s\n", error.c_str());
      pump.stop();
      return 1;
    }
    std::printf("control: http://%s:%d/\n", options.controlBind.c_str(),
                control.port());
  }

  // Both transports exist now, so .NET has done its signal-handler damage
  // and ours stick.
  installStopHandlers();

  auto lastReport = std::chrono::steady_clock::now();
  while (!stopRequested() && pump.running()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(5)) {
      const Pump::Status status = pump.status();
      std::printf("pump: %llu frames, %.1f fps\n",
                  (unsigned long long)status.frames, status.fps);
      std::fflush(stdout);
      lastReport = now;
    }
  }

  control.stop();
  pump.stop();
  const Pump::Status status = pump.status();
  std::printf("pump: stopped after %llu frames\n",
              (unsigned long long)status.frames);
  return 0;
}

}  // namespace oxbow
