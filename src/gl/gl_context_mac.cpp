// CGL offscreen context.
//
// CGL rather than NSOpenGLContext because no window or view ever exists:
// CGLCreateContext with no drawable is the documented way to get a headless
// context, and it needs no Objective-C or main-thread involvement, so the
// frame pump can own the context on its own thread.

#if defined(__APPLE__)

#include <OpenGL/CGLTypes.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include "gl/gl_context.h"

namespace oxbow {
namespace {

class CglContext final : public GlContext {
 public:
  ~CglContext() override {
    if (context_) {
      CGLSetCurrentContext(nullptr);
      CGLDestroyContext(context_);
    }
  }

  bool init(std::string& error) {
    // Core 4.1: the highest macOS offers and what Resolume hosts with.
    const CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile,
        static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core),
        kCGLPFAAccelerated,
        kCGLPFAAllowOfflineRenderers,
        static_cast<CGLPixelFormatAttribute>(0),
    };
    CGLPixelFormatObj pixelFormat = nullptr;
    GLint virtualScreens = 0;
    CGLError err = CGLChoosePixelFormat(attrs, &pixelFormat, &virtualScreens);
    if (err != kCGLNoError || !pixelFormat) {
      error = std::string("CGLChoosePixelFormat failed: ") + CGLErrorString(err);
      return false;
    }
    err = CGLCreateContext(pixelFormat, nullptr, &context_);
    CGLDestroyPixelFormat(pixelFormat);
    if (err != kCGLNoError || !context_) {
      error = std::string("CGLCreateContext failed: ") + CGLErrorString(err);
      return false;
    }
    return true;
  }

  bool makeCurrent() override {
    return CGLSetCurrentContext(context_) == kCGLNoError;
  }

  void doneCurrent() override { CGLSetCurrentContext(nullptr); }

  std::string versionString() override {
    const GLubyte* version = glGetString(GL_VERSION);
    return version ? reinterpret_cast<const char*>(version) : "";
  }

 private:
  CGLContextObj context_ = nullptr;
};

}  // namespace

std::unique_ptr<GlContext> GlContext::create(std::string& error) {
  auto context = std::make_unique<CglContext>();
  if (!context->init(error)) return nullptr;
  return context;
}

}  // namespace oxbow

#endif  // __APPLE__
