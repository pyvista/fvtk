include("${CMAKE_CURRENT_LIST_DIR}/_base.cmake")

# Full backend build: X/GLX + EGL + OSMesa, matching the stock PyPI vtk wheel.
# Runtime backend selection is available since VTK 9.4+ (GLAD replaced GLEW):
# EGL for GPU offscreen (the production viewer), OSMesa for pure-software
# offscreen, and X/GLX so the wheel renders on an Xvfb display exactly the way
# the stock wheel does (the standard pyvista/setup-headless-display CI + dev
# path).
#
# The DEFAULT render window must be the on-screen (X) one, NOT headless, to match
# stock vtk: with a headless default the wheel picks the EGL render window
# whenever VTK_DEFAULT_OPENGL_WINDOW is unset, which needs a GPU and segfaults on
# a GPU-less Xvfb runner (all of CI + dev) instead of software-rendering via
# X/GLX + llvmpipe. Compiling X back in (above) is not enough on its own -- the
# headless default still overrides it. So keep the default on-screen and let the
# headless production deployments select their backend explicitly via the
# VTK_DEFAULT_OPENGL_WINDOW env var (the GPU profiles already set
# vtkEGLRenderWindow; the CPU profile sets it too).
set(VTK_DEFAULT_RENDER_WINDOW_HEADLESS False CACHE BOOL "")
set(VTK_OPENGL_HAS_EGL True CACHE BOOL "")
set(VTK_OPENGL_HAS_OSMESA True CACHE BOOL "")
set(VTK_USE_COCOA False CACHE BOOL "")
set(VTK_USE_X True CACHE BOOL "")

set(VTK_DIST_NAME_SUFFIX "" CACHE STRING "")

# Canonical CoDim VTK wheel: the slim, performance-optimized base build with no
# ray-tracing extras. This is the wheel CoDim installs by default and the one
# the macOS build ships. The +codim.<stamp> local-version segment keeps it from
# colliding with the stock PyPI `vtk` wheel and makes each rebuild
# republishable to the codim devpi index. ospray/anari add their own variant
# tag on top of this stamp. CODIM_VERSION_STAMP comes from _base.cmake.
set(VTK_VERSION_SUFFIX "+codim.${CODIM_VERSION_STAMP}" CACHE STRING "" FORCE)
