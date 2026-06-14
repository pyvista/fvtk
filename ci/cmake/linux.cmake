# fvtk CI init-cache: Linux x86_64 wheel.
#
# Reuses fvtk-config/minimal.cmake (the deny-by-default PyVista closure +
# wheel-build knobs). minimal.cmake already configures the Linux rendering
# stack we want here: X/GLX + EGL + OSMesa, headless default OFF. We only add
# the bits the local build-fvtk.sh applies outside the init-cache (.pyi off is
# already in minimal; pip/wheel handled by the workflow).
#
# Rendering deps (libGL/EGL/OSMesa/X11) come from the manylinux container's
# system packages installed in the workflow (yum). VTK discovers them on the
# default CMAKE_PREFIX_PATH.

include("${CMAKE_CURRENT_LIST_DIR}/../../fvtk-config/minimal.cmake")
