#pragma once

// The one place that knows which GL header each platform uses. Matches what
// the FFGL SDK does internally (FFGL.h includes gl3.h on macOS and GLEW on
// Windows/Linux), so host code and plugin ABI agree.

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif
