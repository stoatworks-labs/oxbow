cmake_minimum_required(VERSION 3.21)

project(oxbow VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(oxbow
  src/app/main.cpp
  src/app/pump.cpp
  src/core/dylib.cpp
  src/ffgl/ffgl_host.cpp
  src/io/ndi.cpp
  src/io/omt.cpp
  src/io/video_io.cpp
)

if(APPLE)
  target_sources(oxbow PRIVATE src/gl/gl_context_mac.cpp)
elseif(WIN32)
  target_sources(oxbow PRIVATE src/gl/gl_context_win.cpp)
endif()

target_include_directories(oxbow PRIVATE
  src
  third_party/ffgl/source/lib
  third_party/ndi/include
  third_party/omt
)

if(APPLE)
  target_compile_definitions(oxbow PRIVATE GL_SILENCE_DEPRECATION)
  target_link_libraries(oxbow PRIVATE "-framework OpenGL")
elseif(WIN32)
  # The FFGL SDK's own vendored GLEW: headers plus a static MSVC x64 lib.
  set(GLEW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffgl/deps/glew-2.1.0)
  target_include_directories(oxbow PRIVATE ${GLEW_ROOT}/include)
  target_compile_definitions(oxbow PRIVATE GLEW_STATIC NOMINMAX)
  target_link_libraries(oxbow PRIVATE
    ${GLEW_ROOT}/lib/Release/x64/glew32s.lib
    opengl32
    user32
    gdi32
  )
  # Sources are UTF-8; MSVC guesses the local codepage without this.
  target_compile_options(oxbow PRIVATE /utf-8)
endif()
