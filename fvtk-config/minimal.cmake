# fvtk MINIMAL profile — the lightest possible PyVista-parity VTK.
#
# Deny-by-default module policy (only PyVista's measured closure, ~84 modules
# vs ~160 for BUILD_ALL) + the production-quality knobs from the CoDim config
# we want to keep (KITS, Python wrapping, EGL+OSMesa+X rendering, logging-on).
#
# Self-contained: does NOT include _base.cmake / _modules.cmake (those turn
# BUILD_ALL_MODULES ON). LTO is OFF here for fast local iteration; flip it ON
# for production wheels.
#
# Configure with:  cmake -S <src> -B <build> -C fvtk-config/minimal.cmake ...

# --- module policy: deny-by-default, enable only PyVista's closure -----------
include("${CMAKE_CURRENT_LIST_DIR}/_modules_minimal.cmake")

# --- Lever A: skip Python-wrapping of pyvista-unused classes (keep C++) -------
# Defines FVTK_NOWRAP_CLASSES; the hook in CMake/vtkModule.cmake demotes each
# from wrapped CLASSES to NOWRAP_CLASSES. Drops ~1500 wrapper compiles + codegen.
include("${CMAKE_CURRENT_LIST_DIR}/_nowrap_classes.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/_nocompile_classes.cmake")

# --- wrapper-unity: batch generated *Python.cxx into chunked unity TUs ----------
# ~48% less wrapper-compile CPU at -O3 by amortizing the shared vtkPython*.h parse.
# CACHE so the value reaches vtk_module_wrap_python's function scope. Emitted code is
# byte-identical (same generated wrappers, just concatenated) -> API/ABI unchanged.
set(FVTK_WRAP_UNITY ON CACHE BOOL "fvtk: batch Python wrappers into unity TUs")
set(FVTK_WRAP_UNITY_CHUNK 32 CACHE STRING "fvtk: wrappers per unity TU")

# --- build hygiene -----------------------------------------------------------
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "")
set(VTK_BUILD_TESTING OFF CACHE BOOL "")
set(VTK_BUILD_DOCUMENTATION OFF CACHE BOOL "")
set(VTK_BUILD_EXAMPLES OFF CACHE BOOL "")
set(VTK_DEBUG_LEAKS OFF CACHE BOOL "")
set(VTK_ENABLE_REMOTE_MODULES OFF CACHE BOOL "")

# LTO is handled manually below via -flto=auto (parallel WPA) so we control the
# job count and keep it composing with the gold/ICF link flags; leave CMake's
# IPO module off to avoid double-injecting -flto.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)

# --- production rendering / runtime flags (from CoDim _base.cmake) -----------
set(VTK_ENABLE_KITS ON CACHE BOOL "")            # ~modules -> ~kits: faster startup, smaller
set(VTK_LEGACY_REMOVE ON CACHE BOOL "")
set(VTK_REPORT_OPENGL_ERRORS OFF CACHE BOOL "")
set(VTK_ENABLE_LOGGING ON CACHE BOOL "")         # pyvista pv.vtk_verbosity() needs it ON
set(VTK_OPENGL_ENABLE_STREAM_ANNOTATIONS OFF CACHE BOOL "")
# Array dispatch: PyVista only ever constructs AOS arrays (numpy_to_vtk -> vtkFloatArray
# etc.), never SOA/ScaledSOA. Enabling SOA+ScaledSOA triples the vtkArrayDispatch typelist
# (14 -> 42 types), instantiating every Dispatch* worker across 3x the array types in the
# big Filters/Common kits — major binary bloat for zero PyVista benefit. OFF = VTK's own
# default; unmatched types still work via the virtual vtkDataArray fallback.
set(VTK_DISPATCH_SCALED_SOA_ARRAYS OFF CACHE BOOL "")
set(VTK_DISPATCH_SOA_ARRAYS OFF CACHE BOOL "")

# Link-time dead-code elimination: emit per-function/data sections and let the linker drop
# unreachable ones. Safe with VTK's -fvisibility=hidden (only exported symbols are GC roots;
# factory/virtual/wrapper paths all go through exported symbols). Removes real code and
# stacks with --strip-all. Applies to SHARED (kits) and MODULE (Python wrappers).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections" CACHE STRING "" FORCE)
# Lever D: --hash-style=gnu emits only the modern .gnu.hash symbol table and
# drops the legacy SysV .hash section (stock VTK ships BOTH across ~140 .so).
# Free and universally safe on glibc (the only fvtk target); ~0.27 MiB measured.
set(_fvtk_ldflags "-Wl,--gc-sections -Wl,--hash-style=gnu")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_fvtk_ldflags}" CACHE STRING "" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${_fvtk_ldflags}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} ${_fvtk_ldflags}" CACHE STRING "" FORCE)
unset(_fvtk_ldflags)

# === SPEED levers (campaign pivot 2026-06-15: make everything PyVista calls fast) ===
# These never drop or slow any module; they make the compiled code faster (and,
# as a side effect, LTO also makes it smaller). ISA floor is left at baseline
# x86-64 (no -march) for maximum wheel portability — chosen deliberately.
#
# Speed-1: -fno-semantic-interposition. By default a shared lib must assume any
# exported (default-visibility) symbol could be interposed at load time (LD_PRELOAD),
# so GCC can't inline or devirtualize across those calls and routes them through
# the PLT. A self-contained viz wheel is never interposed in its own internals,
# so this is safe and lets the compiler inline/devirtualize within each .so —
# a real win for VTK's virtual-dispatch + template-heavy hot paths. Free: no
# portability or build-time cost (CPython itself ships with this). Toggle with
# FVTK_SEMINTERP=0.
if (NOT "$ENV{FVTK_SEMINTERP}" STREQUAL "0")
  set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -fno-semantic-interposition" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-semantic-interposition" CACHE STRING "" FORCE)
endif ()

# Speed-2: LTO (GCC's parallel-WPA analogue of ThinLTO). -flto=auto streams the
# whole-program analysis across nproc jobs so cross-TU inlining + devirtualization
# happens without the serial-LTO time blowup (~30-40 min cold here vs ~13). The
# optimization level that drives LTO's inlining is the one on the LINK line, so we
# add -O3 there too. Composes with gold/--icf=all (ICF folds the post-LTO objects)
# and --gc-sections. LTO also supersedes the per-TU wrapper -Oz size lever (link-
# time -O3 governs), which is fine under a speed-first mandate. Toggle FVTK_LTO=0
# for fast iteration builds.
if (NOT "$ENV{FVTK_LTO}" STREQUAL "0")
  set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -flto=auto -fno-fat-lto-objects" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto=auto -fno-fat-lto-objects" CACHE STRING "" FORCE)
  set(_fvtk_lto_link "-flto=auto -O3 -fno-semantic-interposition")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_fvtk_lto_link}" CACHE STRING "" FORCE)
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${_fvtk_lto_link}" CACHE STRING "" FORCE)
  set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} ${_fvtk_lto_link}" CACHE STRING "" FORCE)
  unset(_fvtk_lto_link)
endif ()

# Identical Code Folding (gold --icf=all). Folds byte-for-byte identical
# functions and ships a single copy: VTK's heavy template instantiation and the
# generated Python-wrapper boilerplate emit a lot of these. Complements
# --gc-sections above (which drops UNREACHABLE code); ICF drops code that is
# reached but DUPLICATED. Measured -10% wheel (47.1 -> 42.2 MiB) on top of the
# other levers. --icf=all (vs --icf=safe) also folds address-taken functions, so
# it can break code that relies on two functions having distinct addresses;
# validated parity-green against PyVista's core+plotting suite (differential
# baseline-vs-ICF run: byte-identical outcomes, 0 regressions). Toggle off with
# `FVTK_ICF=0 ./build-fvtk.sh` for an A/B baseline or if a function-pointer-
# identity issue surfaces (gold --icf=safe is the strictly-weaker fallback).
# Linux/gold only; needs binutils (declared in shell.nix).
#
# Read from the environment, NOT a cache option: this is a `-C` initial-cache
# file, processed before -D args are applied and before project(), so a
# -DFVTK_ICF=OFF cache var would be too late to gate the append below (and
# CMAKE_SYSTEM_NAME isn't set yet either). $ENV{} is available now and matches
# the repo's env-knob idiom (FVTK_STRIP, FAST). Default ON when unset.
if(NOT DEFINED ENV{FVTK_ICF} OR NOT "$ENV{FVTK_ICF}" STREQUAL "0")
  set(_fvtk_icf "-fuse-ld=gold -Wl,--icf=all")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_fvtk_icf}" CACHE STRING "" FORCE)
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${_fvtk_icf}" CACHE STRING "" FORCE)
  set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} ${_fvtk_icf}" CACHE STRING "" FORCE)
  unset(_fvtk_icf)
endif()

set(VTK_PYTHON_FULL_THREADSAFE ON CACHE BOOL "")
set(VTK_NO_PYTHON_THREADS OFF CACHE BOOL "")

# --- Python wheel ------------------------------------------------------------
set(VTK_WHEEL_BUILD ON CACHE BOOL "")
set(VTK_INSTALL_SDK ON CACHE BOOL "")
set(VTK_WRAP_PYTHON YES CACHE BOOL "")
# .pyi type stubs are dev-only IDE hints, not needed at runtime. OFF for the
# lightest build; flip ON if shipping editor type-completion parity matters.
set(VTK_BUILD_PYI_FILES OFF CACHE BOOL "")
set(VTK_USE_PCH OFF CACHE BOOL "")
set(VTK_RELOCATABLE_INSTALL ON CACHE BOOL "")
# Serialization OFF: this nightly has no SerializationManager/WebAssembly
# session modules, so VTK_WRAP_SERIALIZATION=ON makes the resolver require
# them and configure fails. (Same fix as fast.cmake.)
set(VTK_WRAP_SERIALIZATION OFF CACHE BOOL "")

# --- rendering backends: X/GLX + EGL + OSMesa, matching the stock pyvista wheel
set(VTK_DEFAULT_RENDER_WINDOW_HEADLESS False CACHE BOOL "")
set(VTK_OPENGL_HAS_EGL True CACHE BOOL "")
set(VTK_OPENGL_HAS_OSMESA True CACHE BOOL "")
set(VTK_USE_COCOA False CACHE BOOL "")
set(VTK_USE_X True CACHE BOOL "")

# Independent `fvtk` distribution: the Python package installs as `fvtk/` (not
# `vtkmodules/`) and the wheel's dist name is `fvtk` (set in setup.py.in), so it
# coexists with a stock `vtk`/`vtkmodules` install instead of clobbering it.
# No dist suffix on top of that.
set(VTK_DIST_NAME_SUFFIX "" CACHE STRING "")
