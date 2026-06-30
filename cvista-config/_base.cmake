# PEP 440 local-version stamp consumed by the variant CMake presets'
# VTK_VERSION_SUFFIX. Build wrappers default this to the source commit's UTC
# timestamp so all platform wheels from one commit share a package version.
# CODIM_VERSION_STAMP may still be set explicitly for rebuilds.
if(NOT DEFINED CODIM_VERSION_STAMP)
    if(DEFINED ENV{CODIM_VERSION_STAMP} AND NOT "$ENV{CODIM_VERSION_STAMP}" STREQUAL "")
        set(CODIM_VERSION_STAMP "$ENV{CODIM_VERSION_STAMP}")
    else()
        string(TIMESTAMP CODIM_VERSION_STAMP "%Y%m%d%H%M%S" UTC)
    endif()
endif()

set(VTK_BUILD_TESTING OFF CACHE BOOL "")
set(VTK_BUILD_DOCUMENTATION OFF CACHE BOOL "")
set(VTK_BUILD_EXAMPLES OFF CACHE BOOL "")

set(VTK_LINKER_FATAL_WARNINGS ON CACHE BOOL "")
set(VTK_ENABLE_EXTRA_BUILD_WARNINGS ON CACHE BOOL "")
set(VTK_ENABLE_EXTRA_BUILD_WARNINGS_EVERYTHING ON CACHE BOOL "")

set(CMAKE_BUILD_TYPE "Release" CACHE STRING "")
set(VTK_DEBUG_LEAKS OFF CACHE BOOL "")

# Link-time optimization across translation units. ~5-10% runtime speedup on
# hot template-heavy filters and 10-20% binary-size reduction. Costs ~2-3×
# longer build (~1.5 hrs total). Worth it for production wheels.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON CACHE BOOL "" FORCE)

# SMP (shared-memory parallelism) backend stays at STDThread (VTK's default
# in recent wheels). TBB integration was attempted and yielded undefined-
# symbol ImportErrors at .pyi-generation time: Viskores' libviskores_cont
# expects oneTBB 2022.x symbols (`wait_tree_vertex_interface`) but at runtime
# the bundled OSPRay/oneTBB resolution picked an older libtbb. Resolving
# this would need either patching Viskores' TBB version or bundling a
# single libtbb that both VTK and OSPRay use. Tracked in #4; postmortem in
# docs/TBB_INTEGRATION.md.

# Production-rendering performance flags.
# See docs/BUILD_FLAGS.md for the rationale and measured impact of each flag.
set(VTK_ENABLE_KITS ON CACHE BOOL "")                       # ~150 shared libs -> ~15 kits: faster startup, smaller binary
set(VTK_LEGACY_REMOVE ON CACHE BOOL "")                     # strips deprecated API
set(VTK_REPORT_OPENGL_ERRORS OFF CACHE BOOL "")             # no per-GL-call glGetError()
# VTK_ENABLE_LOGGING must stay ON: with it OFF, vtkLogger::GetCurrentVerbosityCutoff()
# returns -10 (sentinel for "logging disabled"), which pyvista's
# pv.vtk_verbosity() doesn't recognize — cascading a KeyError through
# every single test's teardown fixture. See pyvista tests/conftest.py:143.
set(VTK_ENABLE_LOGGING ON CACHE BOOL "")
set(VTK_OPENGL_ENABLE_STREAM_ANNOTATIONS OFF CACHE BOOL "") # no glPushDebugGroup CPU overhead
set(VTK_DISPATCH_SCALED_SOA_ARRAYS ON CACHE BOOL "")        # zero-copy dispatch for scaled arrays

# Python GIL + threading (pyvista-stream uses a dedicated asyncio render thread).
# See codim/packages/pyvista-stream/docs/architecture/THREADING.md.
set(VTK_PYTHON_FULL_THREADSAFE ON CACHE BOOL "")            # lock GIL around all Python C API calls (default ON since 9.4)
# Keep VTK_UNBLOCKTHREADS hints active. Coverage in 9.6.2: Render() releases the
# GIL (vtkWindow::Render carries the hint, wrappers merge inherited hints; patch
# 0012 adds the explicit hint + a regression test), the vtkAlgorithm::Update
# family is hinted, and vtkImageWriter/vtkXMLWriterBase Write() are hinted — but
# vtkWriter::Write() and vtkRenderWindowInteractor::Render() are NOT.
set(VTK_NO_PYTHON_THREADS OFF CACHE BOOL "")

# Wheel configuration
set(VTK_INSTALL_SDK ON CACHE BOOL "")  # wheels turn this off by default
set(VTK_WHEEL_BUILD ON CACHE BOOL "")
set(VTK_WRAP_PYTHON YES CACHE BOOL "")
set(VTK_WRAP_SERIALIZATION ON CACHE BOOL "")
set(VTK_MODULE_ENABLE_VTK_PythonInterpreter NO CACHE STRING "")
set(VTK_BUILD_PYI_FILES ON CACHE BOOL "")
set(VTK_USE_PCH OFF CACHE BOOL "")
set(VTK_RELOCATABLE_INSTALL ON CACHE BOOL "")
set(VTK_DISPATCH_SOA_ARRAYS ON CACHE BOOL "")

include("${CMAKE_CURRENT_LIST_DIR}/_modules.cmake")
