# fvtk CI init-cache: macOS arm64 wheel (runner: macos-15, Apple Silicon).
#
# Reuses fvtk-config/minimal.cmake for the module closure + wheel knobs, but
# overrides the rendering backend: macOS uses Cocoa + system OpenGL, NOT
# X/GLX/EGL/OSMesa. These vars are set BEFORE the include so they win (the
# include's matching `set(... CACHE)` are non-FORCE and become no-ops once the
# cache entry already exists).

set(VTK_USE_COCOA ON CACHE BOOL "")
set(VTK_USE_X OFF CACHE BOOL "")
set(VTK_OPENGL_HAS_EGL OFF CACHE BOOL "")
set(VTK_OPENGL_HAS_OSMESA OFF CACHE BOOL "")
set(VTK_DEFAULT_RENDER_WINDOW_HEADLESS OFF CACHE BOOL "")

# Apple Silicon wheel platform tag: macosx_11_0_arm64 (matches Kitware's 9.6
# arm64 wheels and PyVista's macOS-arm64 support).
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

include("${CMAKE_CURRENT_LIST_DIR}/../../fvtk-config/minimal.cmake")
