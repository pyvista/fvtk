# fvtk CI init-cache: Windows x86_64 wheel (runner: windows-latest, MSVC).
#
# Reuses fvtk-config/minimal.cmake for the module closure + wheel knobs, but
# overrides the rendering backend: Windows uses the Win32 OpenGL backend, NOT
# X/GLX/EGL/OSMesa. Set BEFORE the include so these win over the non-FORCE
# cache entries in minimal.cmake.

set(VTK_USE_COCOA OFF CACHE BOOL "")
set(VTK_USE_X OFF CACHE BOOL "")
set(VTK_OPENGL_HAS_EGL OFF CACHE BOOL "")
set(VTK_OPENGL_HAS_OSMESA OFF CACHE BOOL "")
set(VTK_DEFAULT_RENDER_WINDOW_HEADLESS OFF CACHE BOOL "")

# Match VTK's own wheel config: drop the glut-backed RenderingExternal and the
# MFC GUI helper (no MFC in CI). VideoForWindows / MediaFoundation are left at
# their defaults.
set(VTK_MODULE_ENABLE_VTK_RenderingExternal NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_GUISupportMFC NO CACHE STRING "")

include("${CMAKE_CURRENT_LIST_DIR}/../../fvtk-config/minimal.cmake")
