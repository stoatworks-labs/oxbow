cmake_minimum_required(VERSION 3.21)

project(oxbow VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(oxbow
  src/app/main.cpp
  src/app/pump.cpp
  src/core/dylib.cpp
  src/ffgl/ffgl_host.cpp
  src/gl/gl_context_mac.cpp
  src/io/ndi.cpp
)

target_include_directories(oxbow PRIVATE
  src
  third_party/ffgl/source/lib
  third_party/ndi/include
  third_party/omt
)

if(APPLE)
  target_compile_definitions(oxbow PRIVATE GL_SILENCE_DEPRECATION)
  target_link_libraries(oxbow PRIVATE "-framework OpenGL")
endif()
