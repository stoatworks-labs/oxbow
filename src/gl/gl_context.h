#pragma once

#include <memory>
#include <string>

namespace oxbow {

/// An offscreen OpenGL context suitable for hosting FFGL plugins.
///
/// FFGL 2.x plugins are written against core-profile OpenGL (the SDK includes
/// gl3.h on macOS), so the context must be core profile — 4.1 on macOS, which
/// is both the ceiling there and the version Resolume hosts with, making it
/// the least surprising environment for a plugin that was only ever tested in
/// Resolume. No window is involved: all rendering goes to FBOs.
class GlContext {
 public:
  virtual ~GlContext() = default;

  /// Makes this context current on the calling thread. All FFGL calls that
  /// touch GL (instantiate, process, deinstantiate) must happen with the
  /// context current, on one thread.
  virtual bool makeCurrent() = 0;
  virtual void doneCurrent() = 0;

  /// GL_VERSION string, for logging. Valid after makeCurrent().
  virtual std::string versionString() = 0;

  /// Creates the platform context, or nullptr with `error` set.
  static std::unique_ptr<GlContext> create(std::string& error);
};

}  // namespace oxbow
