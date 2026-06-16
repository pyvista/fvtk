# fvtk CI init-cache: macOS arm64 (Apple Silicon) wheel.
#   Runner: GitHub Actions `macos-14` (Apple Silicon / arm64).
#   Toolchain: AppleClang (Xcode) + cmake + ninja (NOT nix / NOT GCC).
#
# Reuses fvtk-config/minimal.cmake for the deny-by-default PyVista module closure
# + the wheel-build knobs. The patched minimal.cmake already detects macOS
# (CMAKE_HOST_APPLE / FVTK_TARGET_OS=macos) and:
#   - selects the clang ThinLTO (-flto=thin) path instead of GCC -flto=auto,
#   - drops the gold/ICF + GNU-ld (--gc-sections/--hash-style) levers and uses
#     -Wl,-dead_strip (ld64) instead,
#   - sets the rendering backend to Cocoa + system OpenGL (EGL/OSMesa/X OFF) so
#     it does NOT trip VTK's APPLE+EGL FATAL_ERROR (CMake/vtkOpenGLOptions.cmake:54).
#
# We still set the backend + arch vars HERE, BEFORE the include, both as
# belt-and-suspenders (these win because minimal.cmake's matching set()s are
# non-FORCE no-ops once the cache entry exists) and so this file is correct even
# if someone reorders the guards in minimal.cmake later.

# Rendering backend: Cocoa + native GL. ARM-only, no EGL/OSMesa/X on macOS.
set(VTK_USE_COCOA              ON  CACHE BOOL "")
set(VTK_USE_X                  OFF CACHE BOOL "")
set(VTK_OPENGL_HAS_EGL         OFF CACHE BOOL "")   # APPLE+EGL is a FATAL_ERROR in VTK
set(VTK_OPENGL_HAS_OSMESA      OFF CACHE BOOL "")
set(VTK_DEFAULT_RENDER_WINDOW_HEADLESS OFF CACHE BOOL "")

# Apple Silicon ONLY. arm64 + the 11.0 deployment target yield the
# `macosx_11_0_arm64` wheel platform tag (matching Kitware's published 9.6 arm64
# wheels and PyVista's macOS-arm64 support). NO universal2, NO x86_64.
set(CMAKE_OSX_ARCHITECTURES      "arm64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET  "11.0"  CACHE STRING "")

include("${CMAKE_CURRENT_LIST_DIR}/../../fvtk-config/minimal.cmake")
