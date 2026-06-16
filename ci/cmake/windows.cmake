# fvtk CI init-cache: Windows x86_64 wheel (runner: windows-latest, MSVC + Ninja).
#
# Reuses fvtk-config/minimal.cmake for the module closure + wheel knobs, but:
#   1. Forces the MSVC path of minimal.cmake's toolchain-lever gate. A `-C`
#      init-cache file is read before project()/the compiler probe, so WIN32 and
#      CMAKE_CXX_COMPILER_ID are not yet set when minimal.cmake's GNU/MSVC gate
#      runs. FVTK_FORCE_MSVC makes that gate pick the MSVC branch (/GL+/LTCG,
#      /Gy /Gw, /OPT:REF,ICF) and skip the GCC/gold/Linux-only flags
#      (-flto=auto, -fuse-ld=gold --icf=all, --hash-style=gnu, etc.) that MSVC
#      `cl`/`link` reject.
#   2. Overrides the rendering backend: Windows uses the native Win32 WGL
#      OpenGL backend, NOT X/GLX/EGL/OSMesa. This matches the stock pyvista
#      Windows wheel (native OpenGL). Set BEFORE the include so they win over
#      the non-FORCE render cache entries in minimal.cmake.
set(FVTK_FORCE_MSVC ON CACHE BOOL "fvtk: take the MSVC toolchain-lever path")

# --- rendering backend: native Win32 WGL (no EGL/OSMesa/X) --------------------
set(VTK_USE_COCOA OFF CACHE BOOL "")
set(VTK_USE_X OFF CACHE BOOL "")
set(VTK_OPENGL_HAS_EGL OFF CACHE BOOL "")
set(VTK_OPENGL_HAS_OSMESA OFF CACHE BOOL "")
set(VTK_DEFAULT_RENDER_WINDOW_HEADLESS OFF CACHE BOOL "")

# Match VTK's own wheel config: drop the glut-backed RenderingExternal and the
# MFC GUI helper (no MFC in CI). VideoForWindows / MediaFoundation are left at
# their defaults (the minimal module closure does not pull them in).
set(VTK_MODULE_ENABLE_VTK_RenderingExternal NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_GUISupportMFC NO CACHE STRING "")

# Wheel ABI tag: the cp39 .pyd ABI/SOABI is derived from the Python3 interpreter
# cibuildwheel/the workflow points at; nothing platform-specific needed here.

include("${CMAKE_CURRENT_LIST_DIR}/../../fvtk-config/minimal.cmake")
