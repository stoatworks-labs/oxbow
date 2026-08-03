#pragma once

#include <string>

#include "app/pump.h"
#include "control/http_server.h"

namespace oxbow {

/// The control surface: the embedded web page and a small JSON API over the
/// lifted HttpServer, mapped onto one running Pump.
///
///   GET  /                 the control page
///   GET  /api/state        input/output, video status, chain with params
///   POST /api/param?effect=0&name=Mix&value=0.5
///   GET  /api/sources?proto=ndi|omt
///
/// Binds loopback by default; the token story is HttpServer's.
class ControlApi {
 public:
  bool start(Pump& pump, const std::string& bindAddress, int port,
             std::string& error);
  void stop();
  int port() const { return server_.port(); }

 private:
  HttpServer server_;
};

}  // namespace oxbow
