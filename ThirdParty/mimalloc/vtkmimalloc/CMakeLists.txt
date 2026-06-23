# fvtk: build mimalloc as a single SHARED library (libvtkmimalloc.so /
# libvtkmimalloc.dylib / vtkmimalloc.dll) from the upstream single-source
# amalgamation (src/static.c, which #includes every other src/*.c).
#
# WHY SHARED (one instance, library-wide):
#   fvtk routes every kit's global operator new/delete to mimalloc via a small
#   per-kit override TU (Common/Core/vtkFVTKAllocator.cxx, compiled HIDDEN into
#   every fvtk shared library by the post-build injection in the top-level
#   CMakeLists.txt). All of those overrides must forward to the SAME allocator
#   instance: mimalloc keeps its segment/heap metadata in process-global state,
#   so an allocation made through one kit and freed through another only works if
#   both kits call into ONE mimalloc instance. A static-per-kit mimalloc would
#   give each kit/DLL its OWN allocator metadata -> a cross-kit free finds no
#   matching segment -> heap corruption (latent on Linux, certain on Windows
#   where every DLL has its own CRT heap). Building mimalloc as ONE shared lib
#   that every kit links gives one segment map and makes cross-kit/cross-DLL
#   frees consistent.
#
# We compile WITHOUT MI_MALLOC_OVERRIDE: with the override macro undefined,
# src/alloc-override.c is a no-op (it is `#if defined(MI_MALLOC_OVERRIDE)`), so
# this library exports NO malloc/free/operator-new symbols -- only the public
# `mi_*` C API. The global operator new/delete override that actually routes
# VTK's C++ allocations lives in fvtk's own TU (Common/Core/vtkFVTKAllocator.cxx)
# and forwards to mi_malloc_aligned/mi_free. Nothing here ever interposes the
# host CPython allocator (enforced by ci/check-no-alloc-exports.sh, which also
# allows the mi_* exports while forbidding malloc/free/operator new/delete).

set(sources
  src/static.c)

# The public mimalloc API header. static.c pulls in the rest of src/*.c by
# relative path at compile time, so only static.c is a compiled source.
set(headers
  include/mimalloc.h)

vtk_module_add_module(VTK::mimalloc
  SOURCES ${sources}
  HEADERS ${headers}
  HEADERS_SUBDIR "vtkmimalloc")

# static.c does `#include "mimalloc.h"` / `#include "mimalloc/internal.h"` and
# pulls in src/*.c by relative path, so both include/ and src/ must be on the
# include path. include/ is PUBLIC so dependents (the allocator override TU)
# resolve `<mimalloc.h>`; src/ is PRIVATE (only static.c's relative includes).
vtk_module_include(VTK::mimalloc
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Shared, release-quality allocator that EXPORTS ONLY the mi_* C API:
#   MI_SHARED_LIB        -> mi_decl_export resolves (shared-lib linkage spelling)
#   MI_SHARED_LIB_EXPORT -> mi_decl_export == __attribute__((visibility("default")))
#                           on ELF/Mach-O and __declspec(dllexport) on MSVC, so
#                           the mi_* C API is the library's PUBLIC interface (the
#                           per-kit override TU calls mi_malloc_aligned/mi_free
#                           across the .so/.dll boundary, so they MUST be exported
#                           here -- this is the whole point of the single shared
#                           instance). fvtk's global CMAKE_CXX_VISIBILITY_PRESET=
#                           hidden does NOT touch C symbols (no C preset is set),
#                           and the explicit visibility("default") attribute wins
#                           regardless, so mimalloc's own internal (non-mi_decl_
#                           export) symbols stay hidden while mi_* are exported.
# We deliberately DO NOT define MI_MALLOC_OVERRIDE (no malloc/free/new override)
# and DO NOT define MI_STATIC_LIB (we are a DSO now, not a static archive).
vtk_module_definitions(VTK::mimalloc
  PRIVATE
    "MI_SHARED_LIB"
    "MI_SHARED_LIB_EXPORT"
    "MI_BUILD_RELEASE")

# Windows: build a PLAIN shared mimalloc.dll with NO redirection module. By
# default a MI_SHARED_LIB Windows build couples to mimalloc-redirect.dll (its
# src/prim/windows/prim.c does `__declspec(dllimport)` of mi_allocator_init from
# the redirect DLL); without that DLL present, mimalloc.dll fails to load.
# MI_WIN_NOREDIRECT disables that coupling -- the redirect block is skipped and
# the generic no-op allocator-init is used instead. We do NOT use mimalloc's
# malloc/operator-new override or its redirect mechanism at all: fvtk's own
# per-kit C++ operator new/delete override (vtkFVTKAllocator.cxx) is the ONLY
# hook, and it calls the exported mi_* C API directly. The default
# MI_WIN_INIT_USE_CRT_TLS init mode (TLS-callback + CRT-hook based) is
# self-contained and needs no redirect DLL.
if (WIN32)
  vtk_module_definitions(VTK::mimalloc
    PRIVATE
      "MI_WIN_NOREDIRECT")
endif ()
# Dependents (the allocator override TU) must see MI_SHARED_LIB (but NOT
# MI_SHARED_LIB_EXPORT) so that on MSVC mimalloc.h's mi_decl_export resolves to
# __declspec(dllimport) for them. On ELF/Mach-O this is harmless (default vis).
vtk_module_definitions(VTK::mimalloc
  INTERFACE
    "MI_SHARED_LIB")

# mimalloc's unix primitive layer (src/prim/unix/prim.c) needs pthreads, dl, and
# librt (mmap/clock). On Windows mimalloc's windows primitive layer links the
# system libs it needs itself (psapi/advapi32/bcrypt) via its own #pragma
# comment(lib, ...) directives, so no extra link entries are required here.
if (NOT WIN32)
  vtk_module_find_package(PACKAGE Threads)
  vtk_module_link(VTK::mimalloc PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    vtk_module_link(VTK::mimalloc PRIVATE rt)
  endif ()
endif ()
