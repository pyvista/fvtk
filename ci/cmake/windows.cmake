# cvista CI init-cache: Windows x86_64 wheel (runner: windows-latest, MSVC + Ninja).
#
# Reuses cvista-config/minimal.cmake for the module closure + wheel knobs, but:
#   1. Forces the MSVC path of minimal.cmake's toolchain-lever gate. A `-C`
#      init-cache file is read before project()/the compiler probe, so WIN32 and
#      CMAKE_CXX_COMPILER_ID are not yet set when minimal.cmake's GNU/MSVC gate
#      runs. CVISTA_FORCE_MSVC makes that gate pick the MSVC branch (/GL+/LTCG,
#      /Gy /Gw, /OPT:REF,ICF) and skip the GCC/gold/Linux-only flags
#      (-flto=auto, -fuse-ld=gold --icf=all, --hash-style=gnu, etc.) that MSVC
#      `cl`/`link` reject.
#   2. Overrides the rendering backend: Windows uses the native Win32 WGL
#      OpenGL backend, NOT X/GLX/EGL/OSMesa. This matches the stock pyvista
#      Windows wheel (native OpenGL). Set BEFORE the include so they win over
#      the non-FORCE render cache entries in minimal.cmake.
set(CVISTA_FORCE_MSVC ON CACHE BOOL "cvista: take the MSVC toolchain-lever path")

# Pin the compiler to MSVC cl.exe. The GitHub windows runner also ships a mingw
# GCC (C:\mingw64\bin\cc.exe) earlier on PATH, which cmake's Ninja generator will
# otherwise auto-select — and it then chokes on the MSVC-only /GL /Gy /Gw flags
# the msvc toolchain path emits ("compiler is broken"). The workflow runs vcvars
# (ilammy/msvc-dev-cmd) so cl.exe is on PATH; name it explicitly so cmake uses it
# instead of the mingw cc.
set(CMAKE_C_COMPILER   "cl" CACHE STRING "")
set(CMAKE_CXX_COMPILER "cl" CACHE STRING "")

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

include("${CMAKE_CURRENT_LIST_DIR}/../../cvista-config/minimal.cmake")

# --- MSVC link-closure fix: restore classes referenced by kept code ----------
# Lever B (_nocompile_classes.cmake) drops some classes from the build ENTIRELY,
# but a handful are still referenced (::New / ::SafeDownCast) by COMPILED classes:
#   vtkCameraNode, vtkLightNode               <- vtkRendererNode.cxx (SceneGraph)
#   vtkDiscretizableColorTransferFunction     <- vtkWebGLWidget.cxx (WebGLExporter)
# On the GNU/gold Linux link these slip through (gc-sections/LTO elide the dead
# paths), but MSVC's linker correctly fails (LNK2001 unresolved external). The
# referencing classes were found by scanning every kept .cxx for ::New /
# ::SafeDownCast / ::ExtendedNew of a nocompiled class (SerDesHelper refs ignored:
# VTK_WRAP_SERIALIZATION is OFF so those .cxx aren't built). Re-add the referenced
# leaves to the compiled set on the MSVC build ONLY (scoped here, after the
# include, so the proven Linux/macOS builds are byte-unchanged). They remain in
# _nowrap_classes.cmake -> C++ compiled but no Python wrapper. Mirrors the
# precedent in _nocompile_classes.cmake's header (factory-override classes
# restored once their ::New surfaced).
#
# _nocompile_classes.cmake sets CVISTA_NOCOMPILE_CLASSES as a CACHE INTERNAL var,
# and vtkModule.cmake reads the CACHE value — so REMOVE_ITEM on the plain var is
# not enough; re-FORCE the trimmed list back into the cache.
list(REMOVE_ITEM CVISTA_NOCOMPILE_CLASSES
  vtkCameraNode
  vtkLightNode
  vtkDiscretizableColorTransferFunction)
set(CVISTA_NOCOMPILE_CLASSES "${CVISTA_NOCOMPILE_CLASSES}"
    CACHE INTERNAL "cvista: classes dropped from the C++ build (pyvista-unused orphans)" FORCE)
