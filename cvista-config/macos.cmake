include("${CMAKE_CURRENT_LIST_DIR}/_base.cmake")

# macOS uses Cocoa + system OpenGL (no OSMesa/EGL). No OSPRay / ANARI on macOS:
# Intel RenderKit ships no arm64 macOS tarball and ANARI/VisRTX is CUDA-only, so
# the macOS wheel is the slim base build (same module set as cmake/linux.cmake,
# minus the headless GL backends). RenderingRayTracing / RenderingAnari stay OFF
# (the defaults in _modules.cmake).
set(VTK_USE_COCOA ON CACHE BOOL "")
set(VTK_USE_X OFF CACHE BOOL "")

# Apple Silicon only. arm64 + the 11.0 deployment target yield the
# `macosx_11_0_arm64` wheel platform tag VTK's setup.py emits, matching the
# runtime wheels Kitware still publishes for 9.6 (and the wheel-SDK tag scheme
# from docs/WHEEL_SDK.md).
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

# Canonical CoDim VTK wheel stamp — same +codim.<stamp> local-version segment as
# cmake/linux.cmake so the macOS wheel/SDK is republishable to the codim devpi
# index without colliding with the stock PyPI `vtk` wheel. CODIM_VERSION_STAMP
# comes from _base.cmake. (See cmake/linux.cmake for the rationale.)
set(VTK_VERSION_SUFFIX "+codim.${CODIM_VERSION_STAMP}" CACHE STRING "" FORCE)
set(VTK_DIST_NAME_SUFFIX "" CACHE STRING "")
