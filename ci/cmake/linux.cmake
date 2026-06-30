# cvista CI init-cache: Linux x86_64 manylinux_2_28 wheel.
#
# Reuses cvista-config/minimal.cmake (the deny-by-default PyVista closure +
# wheel-build knobs + the LTO / gold-ICF / hash-style / no-semantic-interposition
# levers). minimal.cmake already configures the Linux rendering stack we want:
# X/GLX + EGL + OSMesa with a non-headless default.
#
# Rendering deps (libGL/EGL/OSMesa/X11) come from the manylinux_2_28 container's
# AlmaLinux 8 system packages installed in the workflow (dnf). VTK discovers them
# on the default CMAKE_PREFIX_PATH. Validated configuring inside
# quay.io/pypa/manylinux_2_28_x86_64: glibc 2.28, system GCC 14.2.1, gold + LTO
# plugin all present; the GCC>=12 toolchain ACTIVATES the unity + array-split
# build-time levers (inert on the old manylinux2014/GCC-10.2.1 image).

include("${CMAKE_CURRENT_LIST_DIR}/../../cvista-config/minimal.cmake")
