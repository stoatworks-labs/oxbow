#include "control/control_api.h"

#include <cstdlib>

#include "core/json.h"
#include "control/web_assets.h"
#include "io/video_io.h"

namespace oxbow {
namespace {

json::Value stateJson(Pump& pump) {
  const Pump::Status status = pump.status();
  json::Value root = json::Value::object();

  json::Value video = json::Value::object();
  video.set("source", json::Value(status.inSource));
  video.set("width", json::Value((int)status.width));
  video.set("height", json::Value((int)status.height));
  video.set("frameRate", json::Value(status.frameRate));
  video.set("fps", json::Value(status.fps));
  video.set("frames", json::Value((int64_t)status.frames));
  root.set("video", video);

  json::Value chain = json::Value::array();
  for (const Pump::EffectState& effect : pump.effects()) {
    json::Value entry = json::Value::object();
    entry.set("name", json::Value(effect.name));
    entry.set("path", json::Value(effect.path));
    json::Value params = json::Value::array();
    for (const Pump::ParamState& param : effect.params) {
      json::Value p = json::Value::object();
      p.set("index", json::Value((int)param.index));
      p.set("name", json::Value(param.name));
      p.set("type", json::Value((int)param.type));
      p.set("value", json::Value(param.value));
      p.set("min", json::Value(param.rangeMin));
      p.set("max", json::Value(param.rangeMax));
      params.push(p);
    }
    entry.set("params", params);
    chain.push(entry);
  }
  root.set("chain", chain);
  return root;
}

}  // namespace

bool ControlApi::start(Pump& pump, const std::string& bindAddress, int port,
                       std::string& error) {
  return server_.start(
      bindAddress, port, /*token=*/"",
      [&pump](const HttpServer::Request& request,
              HttpServer::Response& response) {
        if (request.path == "/") {
          response.contentType = "text/html; charset=utf-8";
          response.body = assets::kControlPage;
          return;
        }
        if (request.path == "/api/state") {
          response.json(stateJson(pump).serialize());
          return;
        }
        if (request.path == "/api/param" && request.method == "POST") {
          const size_t effect =
              (size_t)std::strtoul(request.param("effect", "0").c_str(),
                                   nullptr, 10);
          const std::string name = request.param("name");
          const float value = std::strtof(request.param("value", "0").c_str(),
                                          nullptr);
          if (name.empty() || !pump.setParam(effect, name, value)) {
            response.error(404, "no such effect/parameter");
            return;
          }
          response.json("{\"ok\":true}");
          return;
        }
        if (request.path == "/api/sources") {
          std::string listError;
          json::Value list = json::Value::array();
          for (const SourceInfo& source :
               listSources(request.param("proto", "ndi"), 2000, listError)) {
            list.push(json::Value(source.name));
          }
          if (!listError.empty()) {
            response.error(500, listError);
            return;
          }
          response.json(list.serialize());
          return;
        }
        response.error(404, "not found");
      },
      error);
}

void ControlApi::stop() { server_.stop(); }

}  // namespace oxbow
