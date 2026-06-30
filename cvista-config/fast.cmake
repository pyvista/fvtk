# Fast VALIDATION variant of the full linux parity config.
#
# Includes the canonical CoDim linux.cmake (full module set + complete rendering
# stack: EGL + OSMesa + X/GLX + OpenGL2 + FreeType + Matplotlib + Volume +
# Charts) so the wheel is a true 1:1 drop-in for the stock pyvista `vtk` — then
# turns OFF the production-only perf features whose only cost is a much longer
# build:
#   - Interprocedural optimization (LTO): ~2-3x longer build for ~5-10% runtime.
#     Irrelevant when the goal is proving module closure + render parity.
# Everything that affects WHICH modules/symbols ship stays identical to the
# production wheel, so a fast.cmake build validates the exact same surface.
#
# Production wheels: configure with linux.cmake directly (LTO on).
include("${CMAKE_CURRENT_LIST_DIR}/linux.cmake")

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)

# This VTK nightly (9.6.2-2249) ships no Serialization/WebAssembly module dirs,
# but _base.cmake's VTK_WRAP_SERIALIZATION=ON makes the resolver REQUIRE
# SerializationManager + the WebAssembly session modules -> configure fails.
# build-ultralight (which builds cleanly here) leaves it OFF. Force OFF.
set(VTK_WRAP_SERIALIZATION OFF CACHE BOOL "" FORCE)
