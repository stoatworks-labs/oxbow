#pragma once

#include <string>

#include "app/pump.h"

namespace oxbow {

/// Reads a pump configuration from a JSON file:
///
///   {
///     "input":  { "protocol": "ndi", "source": "vMix - Output 3" },
///     "output": { "protocol": "omt", "name": "oxbow" },
///     "control": { "port": 8720 },
///     "chain": [
///       { "plugin": "/path/Effect.bundle", "params": { "Mix": 0.8 } }
///     ]
///   }
///
/// "protocol" defaults to "ndi" on both sides; "chain" may be empty for a
/// passthrough. Returns false with `error` set on unreadable file, bad JSON,
/// or a missing required field.
bool loadConfig(const std::string& path, PumpOptions& options,
                std::string& error);

}  // namespace oxbow
