// WGL offscreen context.
//
// Windows has no CGL equivalent: a GL context needs a device context, which
// needs a window. The window here is 1x1, never shown, and exists only to
// carry the pixel format. Creating a modern core-profile context is the usual
// two-step: a throwaway legacy context first, because
// wglCreateContextAttribsARB can only be resolved while some context is
// current.

#if defined(_WIN32)

#include <GL/glew.h>
#include <GL/wglew.h>
#include <windows.h>

#include "gl/gl_context.h"

namespace oxbow {
namespace {

const wchar_t* const kWindowClass = L"oxbow-gl";

class WglContext final : public GlContext {
 public:
  ~WglContext() override {
    if (context_) {
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(context_);
    }
    if (dc_ && window_) ReleaseDC(window_, dc_);
    if (window_) DestroyWindow(window_);
  }

  bool init(std::string& error) {
    WNDCLASSW windowClass = {};
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassW(&windowClass);// Idempotent; failure means already registered.

    window_ = CreateWindowW(kWindowClass, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                            nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window_) {
      error = "CreateWindow failed";
      return false;
    }
    dc_ = GetDC(window_);

    PIXELFORMATDESCRIPTOR descriptor = {};
    descriptor.nSize = sizeof descriptor;
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW;
    descriptor.iPixelType = PFD_TYPE_RGBA;
    descriptor.cColorBits = 32;
    const int format = ChoosePixelFormat(dc_, &descriptor);
    if (!format || !SetPixelFormat(dc_, format, &descriptor)) {
      error = "no usable pixel format";
      return false;
    }

    HGLRC legacy = wglCreateContext(dc_);
    if (!legacy || !wglMakeCurrent(dc_, legacy)) {
      error = "legacy WGL context failed";
      return false;
    }
    auto createContextAttribs =
        reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
            wglGetProcAddress("wglCreateContextAttribsARB"));
    if (createContextAttribs) {
      const int attribs[] = {
          WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
          WGL_CONTEXT_MINOR_VERSION_ARB, 1,
          WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
          0,
      };
      context_ = createContextAttribs(dc_, nullptr, attribs);
    }
    if (!context_) context_ = legacy;// Pre-3.2 driver; FFGL 2 will not work,
                                     // but versionString() will say why.
    if (context_ != legacy) {
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(legacy);
    }
    if (!wglMakeCurrent(dc_, context_)) {
      error = "wglMakeCurrent failed";
      return false;
    }

    glewExperimental = GL_TRUE;
    const GLenum glewStatus = glewInit();
    if (glewStatus != GLEW_OK) {
      error = std::string("glewInit failed: ") + reinterpret_cast<const char*>(
                                                     glewGetErrorString(glewStatus));
      return false;
    }
    return true;
  }

  bool makeCurrent() override { return wglMakeCurrent(dc_, context_) != FALSE; }

  void doneCurrent() override { wglMakeCurrent(nullptr, nullptr); }

  std::string versionString() override {
    const GLubyte* version = glGetString(GL_VERSION);
    return version ? reinterpret_cast<const char*>(version) : "";
  }

 private:
  HWND window_ = nullptr;
  HDC dc_ = nullptr;
  HGLRC context_ = nullptr;
};

}  // namespace

std::unique_ptr<GlContext> GlContext::create(std::string& error) {
  auto context = std::make_unique<WglContext>();
  if (!context->init(error)) return nullptr;
  return context;
}

}  // namespace oxbow

#endif  // _WIN32
