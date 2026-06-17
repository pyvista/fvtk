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

# --- Lever: Python stable-ABI (abi3 / Py_LIMITED_API) — EXPERIMENTAL ----------
# When ON, the Python wrapper TUs (generated *Python.cxx + WrappingPythonCore)
# are compiled with Py_LIMITED_API set to FVTK_ABI3_VERSION so that ONE wheel
# (cp3x-abi3) is import-compatible across all CPython >= that version, instead
# of the current per-minor cp39..cp313 matrix.
#
# STATUS: the stock VTK wrapper runtime is NOT limited-API clean (it defines
# static PyTypeObject instances and pokes tp_* fields directly; under
# Py_LIMITED_API PyTypeObject is an OPAQUE type, so those TUs fail to compile).
# Turning this ON is therefore a *diagnostic* lever for now: it drives the
# compile so the concrete blocker errors can be collected as the porting
# worklist. See docs/abi3-feasibility.md. Do NOT expect a working wheel yet.
option(FVTK_ABI3 "fvtk: compile Python wrappers against the CPython stable ABI (Py_LIMITED_API). EXPERIMENTAL — see docs/abi3-feasibility.md" OFF)
# 0x030d0000 == CPython 3.13. The limited-API floor; the resulting abi3 wheel
# would be tagged cp313-abi3 and load on 3.13+. Lower it (e.g. 0x03090000 for
# 3.9) only after the runtime is actually ported.
set(FVTK_ABI3_VERSION "0x030d0000" CACHE STRING "fvtk: Py_LIMITED_API value when FVTK_ABI3 is ON (0x030d0000 = CPython 3.13)")

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

# === 3-way toolchain gate for the compiler/linker levers below ================
# All the levers from here through the ICF block are GNU-toolchain + ELF/gold
# specific (-flto=auto/-fno-fat-lto-objects, -ffunction-sections + --gc-sections,
# -fno-semantic-interposition, --hash-style=gnu, -fuse-ld=gold --icf=all, GCC PGO
# via .gcda). They are rejected (or silently ignored) by MSVC `cl`/`link` and by
# AppleClang/ld64. We pick ONE of three toolchain profiles and each lever block
# below substitutes the right spelling:
#   gnu   = GCC + gold on Linux (the shipped wheel) — today's exact flags.
#   apple = AppleClang + ld64 on macOS — ThinLTO (-flto=thin), -Wl,-dead_strip,
#           no gold/--icf/--hash-style/--gc-sections (Mach-O, not ELF).
#   msvc  = MSVC `cl`/`link` on Windows — /GL+/LTCG, /Gy /Gw, /OPT:REF,ICF.
#
# IMPORTANT — the default MUST be "gnu", and we flip away from it only on a
# *positive* MSVC or Apple detection. A `-C` init-cache file is read BEFORE
# project()/the compiler probe, so on the first pass WIN32, MSVC and
# CMAKE_CXX_COMPILER_ID are all empty/unset. If we instead keyed "gnu" off a
# positive GNU match (e.g. CMAKE_CXX_COMPILER_ID MATCHES "GNU") it would evaluate
# false on that first pass *even on Linux* and silently drop every lever from the
# production build. So: assume gnu, switch to msvc/apple only when we positively
# know otherwise. ci/cmake/windows.cmake sets -DFVTK_FORCE_MSVC=ON and
# ci/cmake/macos.cmake sets FVTK_TARGET_OS=macos / runs on an APPLE host so the
# first-pass detection still fires; the MSVC/WIN32/compiler-id checks catch the
# post-project() re-read as belt-and-suspenders.
set(_FVTK_TOOLCHAIN "gnu")
if (FVTK_FORCE_MSVC OR MSVC OR WIN32 OR CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  set(_FVTK_TOOLCHAIN "msvc")
elseif (CMAKE_HOST_APPLE OR "$ENV{FVTK_TARGET_OS}" STREQUAL "macos")
  set(_FVTK_TOOLCHAIN "apple")
endif ()

# Link-time dead-code elimination: emit per-function/data sections and let the linker drop
# unreachable ones. Safe with VTK's -fvisibility=hidden (only exported symbols are GC roots;
# factory/virtual/wrapper paths all go through exported symbols). Removes real code and
# stacks with --strip-all. Applies to SHARED (kits) and MODULE (Python wrappers).
# -ffunction-sections/-fdata-sections are understood by BOTH gcc and clang, so
# they are unconditional on gnu+apple; the MSVC analogue is /Gy /Gw, emitted in
# the msvc lever block below.
if (NOT _FVTK_TOOLCHAIN STREQUAL "msvc")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections" CACHE STRING "" FORCE)
endif ()
# Lever D: --hash-style=gnu emits only the modern .gnu.hash symbol table and
# drops the legacy SysV .hash section (stock VTK ships BOTH across ~140 .so).
# Free and universally safe on glibc (the only fvtk target); ~0.27 MiB measured.
# GNU-ld only: ld64 (macOS) has no .hash/.gnu.hash sections (Mach-O) and PE/COFF
# (Windows) has no ELF symbol hash either, so each gets its own dead-strip below.
if (_FVTK_TOOLCHAIN STREQUAL "gnu")
  set(_fvtk_ldflags "-Wl,--gc-sections -Wl,--hash-style=gnu")
elseif (_FVTK_TOOLCHAIN STREQUAL "apple")
  # macOS / ld64: -dead_strip is the GNU --gc-sections analogue (keys off the
  # per-function/data sections clang emits); --gc-sections/--hash-style are
  # GNU-ld-only and would error under ld64.
  set(_fvtk_ldflags "-Wl,-dead_strip")
else ()
  # msvc: section GC + COMDAT folding is /OPT:REF,ICF on the link line, emitted in
  # the msvc lever block below (it is not a -Wl, flag, so nothing here).
  set(_fvtk_ldflags "")
endif ()
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
# FVTK_SEMINTERP=0. clang accepts -fno-semantic-interposition too, so gnu+apple
# both take it; MSVC has no symbol interposition on PE/COFF (cross-TU inlining is
# already the default) so the flag has no analogue and is skipped there.
if (NOT _FVTK_TOOLCHAIN STREQUAL "msvc" AND NOT "$ENV{FVTK_SEMINTERP}" STREQUAL "0")
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
  if (_FVTK_TOOLCHAIN STREQUAL "gnu")
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -flto=auto -fno-fat-lto-objects" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto=auto -fno-fat-lto-objects" CACHE STRING "" FORCE)
    set(_fvtk_lto_link "-flto=auto -O3 -fno-semantic-interposition")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_fvtk_lto_link}" CACHE STRING "" FORCE)
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${_fvtk_lto_link}" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} ${_fvtk_lto_link}" CACHE STRING "" FORCE)
    unset(_fvtk_lto_link)
  elseif (_FVTK_TOOLCHAIN STREQUAL "apple")
    # AppleClang: ThinLTO. `-flto=thin` is clang's parallel cross-TU LTO (per-TU
    # summary + parallel backend), the direct analogue of GCC's -flto=auto. clang
    # has no -fno-fat-lto-objects (its LTO objects are bitcode by default) and
    # ThinLTO's optimization level is the per-TU -O (already -O3 from Release), so
    # the link line only needs -flto=thin. -fno-semantic-interposition is appended
    # by the seminterp block above (compile-only under ld64).
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -flto=thin" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto=thin" CACHE STRING "" FORCE)
    set(_fvtk_lto_link "-flto=thin")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_fvtk_lto_link}" CACHE STRING "" FORCE)
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${_fvtk_lto_link}" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} ${_fvtk_lto_link}" CACHE STRING "" FORCE)
    unset(_fvtk_lto_link)
  else ()
    # MSVC: /GL (whole-program optimization, the LTO analogue) on compile + /LTCG
    # (link-time code generation) on link. /GL objects also need /LTCG passed to
    # lib.exe for any static archives. The Release -O drives the inlining (no
    # separate -O3-on-link knob as on GCC).
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} /GL" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GL" CACHE STRING "" FORCE)
    set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} /LTCG" CACHE STRING "" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /LTCG" CACHE STRING "" FORCE)
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /LTCG" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} /LTCG" CACHE STRING "" FORCE)
  endif ()
endif ()

# Speed-3: Profile-Guided Optimization (FVTK_PGO=gen | use). Two-phase: an
# instrumented build (gen) records real branch/call frequencies while a training
# workload (PyVista's own test suite + the perf benchmark) runs against it, then
# the final build (use) recompiles guided by that profile — better inlining, hot/
# cold splitting and branch layout than static heuristics. Drives the same build
# dir twice: gen writes .gcda next to each .o (default location); use reads them
# in place, so the object paths MUST match (same BUILD dir for both phases).
#   gen: instrument. -fprofile-update=atomic because VTK's SMPTools run threaded;
#        without atomic the shared counters race and corrupt the profile. Run this
#        phase with FVTK_LTO=0 FVTK_ICF=0 (instrument cleanly; ICF would fold
#        distinct counters together) and unstripped.
#   use: consume. -fprofile-correction tolerates the threaded-profile residue;
#        -Wno-coverage-mismatch keeps stale-counter warnings non-fatal. Stack with
#        FVTK_LTO=1 FVTK_ICF=1 for the shipped wheel (profile guides LTO inlining).
if (NOT _FVTK_TOOLCHAIN STREQUAL "gnu" AND NOT "$ENV{FVTK_PGO}" STREQUAL "")
  # PGO here is GCC's .gcda flow (-fprofile-generate/-use). clang's PGO
  # (-fprofile-instr-generate / llvm-profdata / -fprofile-instr-use) and MSVC's
  # (/GENPROFILE + /USEPROFILE + pgort) are different toolchains entirely and are
  # out of scope for the first macOS/Windows wheels (built with FVTK_PGO unset).
  message(WARNING "FVTK_PGO is GCC-only; ignored on the ${_FVTK_TOOLCHAIN} build.")
elseif ("$ENV{FVTK_PGO}" STREQUAL "gen")
  set(_fvtk_pgo "-fprofile-generate -fprofile-update=atomic")
  set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${_fvtk_pgo}" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_fvtk_pgo}" CACHE STRING "" FORCE)
  # link needs -fprofile-generate too so libgcov is pulled in.
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-generate" CACHE STRING "" FORCE)
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fprofile-generate" CACHE STRING "" FORCE)
  set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} -fprofile-generate" CACHE STRING "" FORCE)
  unset(_fvtk_pgo)
elseif ("$ENV{FVTK_PGO}" STREQUAL "use")
  set(_fvtk_pgo "-fprofile-use -fprofile-correction -Wno-coverage-mismatch")
  set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${_fvtk_pgo}" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_fvtk_pgo}" CACHE STRING "" FORCE)
  unset(_fvtk_pgo)
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
# gold (-fuse-ld=gold) and gold's --icf both exist only on the GNU/ELF toolchain:
# ld64 (macOS) has no --icf (lld's --icf=all would, but the default macOS linker
# is ld64) and MSVC folds via /OPT:ICF in its own block below. So this lever is
# gated to gnu. On apple, ICF is held OFF for the first arm64 wheel (correctness-
# first, no function-pointer-identity risk; -dead_strip already recovers the bulk
# of the size win) — a future macOS ICF can opt into -fuse-ld=lld --icf=all once
# lld is provisioned and validated.
if(_FVTK_TOOLCHAIN STREQUAL "gnu" AND (NOT DEFINED ENV{FVTK_ICF} OR NOT "$ENV{FVTK_ICF}" STREQUAL "0"))
  set(_fvtk_icf "-fuse-ld=gold -Wl,--icf=all")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_fvtk_icf}" CACHE STRING "" FORCE)
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${_fvtk_icf}" CACHE STRING "" FORCE)
  set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} ${_fvtk_icf}" CACHE STRING "" FORCE)
  unset(_fvtk_icf)
endif()

# === MSVC section-GC + COMDAT folding (Windows) ==============================
# The gnu/apple section-GC (-ffunction-sections + --gc-sections/-dead_strip) and
# gold ICF map to MSVC's /Gy /Gw (function+data COMDATs on compile) + /OPT:REF
# (drop unreferenced COMDATs) + /OPT:ICF (fold identical COMDATs) on link. /Gy /Gw
# are cheap and required for /OPT:REF,ICF to have COMDATs to fold, so they apply
# whenever the toolchain is msvc (the LTO /GL+/LTCG above already honoured
# FVTK_LTO=0 separately). No PGO/--hash-style/-fno-semantic-interposition analogue
# on PE/COFF (see the comments on those levers above).
if (_FVTK_TOOLCHAIN STREQUAL "msvc")
  set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} /Gy /Gw" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Gy /Gw" CACHE STRING "" FORCE)
  set(_fvtk_msvc_link "/OPT:REF /OPT:ICF")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_fvtk_msvc_link}" CACHE STRING "" FORCE)
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${_fvtk_msvc_link}" CACHE STRING "" FORCE)
  set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} ${_fvtk_msvc_link}" CACHE STRING "" FORCE)
  unset(_fvtk_msvc_link)
endif ()

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

# --- rendering backends -------------------------------------------------------
# Linux: X/GLX + EGL + OSMesa, matching the stock pyvista wheel.
# macOS: native GL is Cocoa-bound; EGL/OSMesa/X are NOT available and VTK hard
#   errors at configure if VTK_OPENGL_HAS_EGL is ON on APPLE
#   (CMake/vtkOpenGLOptions.cmake FATAL_ERROR). Cocoa + system OpenGL, matching
#   the stock pyvista macOS wheel.
# Windows: native Win32 WGL backend (the default the stock pyvista Windows wheel
#   ships) — no EGL/OSMesa/X.
# These vars are non-FORCE, so a `-C ci/cmake/{macos,windows}.cmake` init-cache
# that sets them BEFORE this include wins; the per-toolchain guard here keeps a
# direct `-C minimal.cmake` configure correct on each platform too.
set(VTK_DEFAULT_RENDER_WINDOW_HEADLESS False CACHE BOOL "")
if (_FVTK_TOOLCHAIN STREQUAL "apple")
  set(VTK_OPENGL_HAS_EGL    False CACHE BOOL "")
  set(VTK_OPENGL_HAS_OSMESA False CACHE BOOL "")
  set(VTK_USE_COCOA         True  CACHE BOOL "")
  set(VTK_USE_X             False CACHE BOOL "")
elseif (_FVTK_TOOLCHAIN STREQUAL "msvc")
  set(VTK_OPENGL_HAS_EGL    False CACHE BOOL "")
  set(VTK_OPENGL_HAS_OSMESA False CACHE BOOL "")
  set(VTK_USE_COCOA         False CACHE BOOL "")
  set(VTK_USE_X             False CACHE BOOL "")
else ()
  set(VTK_OPENGL_HAS_EGL True CACHE BOOL "")
  set(VTK_OPENGL_HAS_OSMESA True CACHE BOOL "")
  set(VTK_USE_COCOA False CACHE BOOL "")
  set(VTK_USE_X True CACHE BOOL "")
endif ()

# Independent `fvtk` distribution: the Python package installs as `fvtk/` (not
# `vtkmodules/`) and the wheel's dist name is `fvtk` (set in setup.py.in), so it
# coexists with a stock `vtk`/`vtkmodules` install instead of clobbering it.
# No dist suffix on top of that.
set(VTK_DIST_NAME_SUFFIX "" CACHE STRING "")
