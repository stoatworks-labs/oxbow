cmake_minimum_required(VERSION 3.21)

project(oxbow VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(OXBOW_VERSION="${PROJECT_VERSION}")

add_executable(oxbow
  src/app/config.cpp
  src/app/main.cpp
  src/app/main_loop.cpp
  src/app/pump.cpp
  src/app/run.cpp
  src/core/dylib.cpp
  src/diag/diag.cpp
  src/core/json.cpp
  src/control/control_api.cpp
  src/control/http_server.cpp
  src/ffgl/ffgl_host.cpp
  src/io/ndi.cpp
  src/io/omt.cpp
  src/io/video_io.cpp
)

if(APPLE)
  target_sources(oxbow PRIVATE src/gl/gl_context_mac.cpp)

  # Syphon output, plus the vendored Syphon server subset (BSD-3; see
  # third_party/syphon/README.md for what was taken and what was left).
  #
  # Those sources are ARC and expect Syphon's own prefix header for SYPHONLOG,
  # neither of which is true of anything else in this repo, so both are set
  # per-file rather than on the target.
  target_sources(oxbow PRIVATE src/io/syphon.mm)
  set(OXBOW_SYPHON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/syphon/Syphon)
  file(GLOB OXBOW_SYPHON_OBJC ${OXBOW_SYPHON_DIR}/*.m)
  file(GLOB OXBOW_SYPHON_C    ${OXBOW_SYPHON_DIR}/*.c)
  target_sources(oxbow PRIVATE ${OXBOW_SYPHON_OBJC} ${OXBOW_SYPHON_C})

  # -w because this is upstream code we do not modify, and it uses
  # kIOSurfaceIsGlobal — which is how Syphon shares a surface between processes
  # at all, and is deprecated with no replacement. Left as a warning it would be
  # noise in every build forever.
  #
  # ARC is Syphon's own setting and applies only to the Objective-C files;
  # SyphonDispatch.c is plain C, where the flag is not merely unnecessary but
  # rejected.
  set_source_files_properties(${OXBOW_SYPHON_OBJC} PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc;-include;${OXBOW_SYPHON_DIR}/Syphon_Prefix.pch;-w")
  set_source_files_properties(${OXBOW_SYPHON_C} PROPERTIES COMPILE_OPTIONS "-w")
elseif(WIN32)
  target_sources(oxbow PRIVATE src/gl/gl_context_win.cpp)

  # Spout output, plus the vendored Spout DirectX sender subset (BSD-2; see
  # third_party/spout/README.md). /w because this is upstream code we do not
  # modify and it builds clean at its own warning level, not ours.
  target_sources(oxbow PRIVATE src/io/spout.cpp)
  set(OXBOW_SPOUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spout/Spout)
  file(GLOB OXBOW_SPOUT_SOURCES ${OXBOW_SPOUT_DIR}/*.cpp)
  target_sources(oxbow PRIVATE ${OXBOW_SPOUT_SOURCES})
  set_source_files_properties(${OXBOW_SPOUT_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
endif()

# --- DeckLink (optional) ------------------------------------------------------
#
# Blackmagic's SDK is not vendored: its licence is not ours to redistribute.
# Point at an extracted copy and the output is compiled in.
#
# A developer machine often has several SDK copies at different versions sitting
# inside other products — this Mac has a 10.11.2 set inside the NDI SDK's
# examples and a 12.2 one inside Unreal's BlackmagicMedia. Anything below 11.0
# lacks IDeckLinkProfileAttributes, so the version is checked rather than
# assumed and the error says what to do about it.
set(DECKLINK_SDK_DIR "" CACHE PATH "Blackmagic DeckLink SDK (enables the decklink output)")
if(DECKLINK_SDK_DIR)
  if(APPLE)
    set(DECKLINK_PLATFORM Mac)
  elseif(WIN32)
    set(DECKLINK_PLATFORM Win)
  else()
    set(DECKLINK_PLATFORM Linux)
  endif()

  find_path(DECKLINK_INCLUDE_DIR DeckLinkAPI.h
    HINTS "${DECKLINK_SDK_DIR}"
    PATH_SUFFIXES "${DECKLINK_PLATFORM}/include" "include/${DECKLINK_PLATFORM}"
                  "${DECKLINK_PLATFORM}" "Include/${DECKLINK_PLATFORM}" "include"
    NO_DEFAULT_PATH)
  if(NOT DECKLINK_INCLUDE_DIR)
    message(FATAL_ERROR "No DeckLinkAPI.h under ${DECKLINK_SDK_DIR}")
  endif()

  file(STRINGS "${DECKLINK_INCLUDE_DIR}/DeckLinkAPIVersion.h" DECKLINK_VERSION_LINE
       REGEX "BLACKMAGIC_DECKLINK_API_VERSION_STRING")
  string(REGEX MATCH "([0-9]+)\\.([0-9]+)" DECKLINK_VERSION "${DECKLINK_VERSION_LINE}")
  if(CMAKE_MATCH_1 AND CMAKE_MATCH_1 LESS 11)
    message(FATAL_ERROR
      "DeckLink SDK at ${DECKLINK_INCLUDE_DIR} is ${DECKLINK_VERSION}; 11.0 or newer "
      "is required (IDeckLinkProfileAttributes). There is often a much older copy "
      "bundled inside another product — point at the real SDK.")
  endif()

  target_sources(oxbow PRIVATE
    src/io/decklink.cpp
    "${DECKLINK_INCLUDE_DIR}/DeckLinkAPIDispatch.cpp")
  set_source_files_properties("${DECKLINK_INCLUDE_DIR}/DeckLinkAPIDispatch.cpp"
    PROPERTIES COMPILE_OPTIONS "-w")
  target_include_directories(oxbow PRIVATE "${DECKLINK_INCLUDE_DIR}")
  target_compile_definitions(oxbow PRIVATE OXBOW_WITH_DECKLINK)
  message(STATUS "DeckLink output: enabled, SDK ${DECKLINK_VERSION} at ${DECKLINK_INCLUDE_DIR}")
else()
  message(STATUS "DeckLink output: disabled (set DECKLINK_SDK_DIR to enable)")
endif()

target_include_directories(oxbow PRIVATE
  src
  third_party/ffgl/source/lib
  third_party/ndi/include
  third_party/omt
)

if(APPLE)
  target_compile_definitions(oxbow PRIVATE GL_SILENCE_DEPRECATION)
  # third_party/syphon is the include root, not .../syphon/Syphon: the vendored
  # sources import <Syphon/SyphonServerBase.h> framework-style, and this is what
  # makes that resolve against the unmodified files.
  target_include_directories(oxbow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/syphon)
  target_link_libraries(oxbow PRIVATE
    "-framework OpenGL"
    "-framework Foundation"
    "-framework IOSurface"
    "-framework CoreFoundation"
    # Syphon's SyphonServerBase uses NSRunningApplication to put the host
    # application's name in its server description. WebLinked got AppKit for
    # free by being a .app; a command-line tool has to ask for it.
    "-framework AppKit")
elseif(WIN32)
  # The FFGL SDK's own vendored GLEW: headers plus a static MSVC x64 lib.
  set(GLEW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffgl/deps/glew-2.1.0)
  target_include_directories(oxbow PRIVATE ${GLEW_ROOT}/include
                                          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spout/Spout)
  target_compile_definitions(oxbow PRIVATE GLEW_STATIC NOMINMAX
                                           OMT_NO_IMPLICIT_LINK)
  target_link_libraries(oxbow PRIVATE
    ${GLEW_ROOT}/lib/Release/x64/glew32s.lib
    opengl32
    # Spout's DirectX sender.
    d3d11
    dxgi
    ws2_32
    dbghelp
    user32
    gdi32
  )
  # Sources are UTF-8; MSVC guesses the local codepage without this.
  target_compile_options(oxbow PRIVATE /utf-8)
endif()
