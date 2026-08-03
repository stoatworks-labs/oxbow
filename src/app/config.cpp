#include "app/config.h"

#include <fstream>
#include <sstream>

#include "core/json.h"

namespace oxbow {

bool loadConfig(const std::string& path, PumpOptions& options,
                std::string& error) {
  std::ifstream file(path);
  if (!file) {
    error = "cannot read " + path;
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();

  std::string parseError;
  auto root = json::parse(buffer.str(), &parseError);
  if (!root) {
    error = path + ": " + parseError;
    return false;
  }

  const json::Value& input = (*root)["input"];
  const json::Value& output = (*root)["output"];
  options.inProtocol = input["protocol"].asString("ndi");
  options.inName = input["source"].asString();
  options.outProtocol = output["protocol"].asString("ndi");
  options.outName = output["name"].asString();
  if (options.inName.empty() || options.outName.empty()) {
    error = path + ": input.source and output.name are required";
    return false;
  }

  const json::Value& control = (*root)["control"];
  options.controlPort = control["port"].asInt(0);
  options.controlBind = control["bind"].asString("127.0.0.1");

  const json::Value& chain = (*root)["chain"];
  for (size_t i = 0; i < chain.size(); ++i) {
    const json::Value& entry = chain.at(i);
    EffectSpec spec;
    spec.path = entry["plugin"].asString();
    if (spec.path.empty()) {
      error = path + ": chain[" + std::to_string(i) + "] has no \"plugin\"";
      return false;
    }
    for (const auto& [name, value] : entry["params"].members())
      spec.sets.emplace_back(name, (float)value.asDouble());
    options.effects.push_back(std::move(spec));
  }
  return true;
}

}  // namespace oxbow
