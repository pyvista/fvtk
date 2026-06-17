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

# --- source-unity: batch each module's *source* .cxx into CMake UNITY_BUILD TUs -
# The 2,511 module source TUs each re-parse the same heavy VTK/STL header stack;
# UNITY_BUILD concatenates several into one TU to amortize that parse (the dominant
# cold-compile cost). Object code is byte-identical -> bit-exact-safe. The hook
# (CMake/vtkModule.cmake, after target_sources) is gated by the env knob
# FVTK_SOURCE_UNITY (default ON here; FVTK_SOURCE_UNITY=0 disables). It excludes
# vtkCommonCore (owned by the array-instantiation TU split, PR #27) plus any
# module listed in FVTK_SOURCE_UNITY_EXCLUDE that does not batch clean. The env
# knob is read at hook time; we default it ON for the production build by exporting
# it from build-fvtk.sh, but ALSO record the intended default + the exclude list
# here so a direct `-C minimal.cmake` configure that sets FVTK_SOURCE_UNITY=1 in
# its env behaves identically.
include("${CMAKE_CURRENT_LIST_DIR}/_source_unity_exclude.cmake")
set(FVTK_SOURCE_UNITY_BATCH 8 CACHE STRING "fvtk: module source .cxx per unity TU")

# --- Lever: Python stable-ABI (abi3 / Py_LIMITED_API) — DEFAULT ON ------------
# When ON (the default), the Python wrapper TUs (generated *Python.cxx +
# WrappingPythonCore) are compiled with Py_LIMITED_API set to FVTK_ABI3_VERSION
# so that ONE wheel (cp312-abi3) is import-compatible across all CPython >= the
# floor, instead of a per-minor cp312..cp314 matrix. The wrapper runtime +
# generator are fully ported to PyType_FromSpec heap types behind FVTK_ABI3; the
# resulting wheel is bit-exact with stock VTK 9.6.2 (maxULP=0, numpy zero-copy
# shared+byte-identical) EXCEPT for the unavoidable type __flags__ divergence
# (HEAPTYPE=1/IMMUTABLETYPE=0 — every limited-API type is a heap type; there is
# no limited-API way to make a static type). See docs/abi3-feasibility.md.
#
# A plain local build (or any single -C minimal.cmake configure) is therefore an
# abi3 build by default. The shipped matrix is a TWO-WHEEL scheme: the cibuildwheel
# backend (ci/cibw/fvtk_backend.py) FORCES FVTK_ABI3 OFF on the cp311 leg (3.11
# cannot be a stable-ABI floor — see below) so 3.11 ships as a normal static wheel,
# and keeps it ON for cp312+ to ship the single cp312-abi3 wheel.
#
# ESCAPE HATCH: configure with -DFVTK_ABI3=OFF (or FVTK_ABI3=0 in the wheel
# backend env) to build the legacy per-version static-type wheel (strict
# byte-for-byte parity incl. __flags__), tagged cp3XX-cp3XX as before.
option(FVTK_ABI3 "fvtk: compile Python wrappers against the CPython stable ABI (Py_LIMITED_API). Default ON (3.12+); the cibuildwheel backend forces OFF on cp311 (static wheel). See docs/abi3-feasibility.md" ON)
# 0x030c0000 == CPython 3.12 — the limited-API floor. The resulting abi3 wheel is
# tagged cp312-abi3 and loads on CPython 3.12+. (3.11 is NOT a valid abi3 floor:
# PyMemberDef and the Py_T_*/Py_READONLY member constants the heap-type wrappers
# emit only entered the stable ABI in 3.12 — gh-93274 — so a 3.11 floor does not
# compile against the genuine 3.11 limited-API headers; 3.11 is served by the
# static cp311 wheel instead.) Raise it (e.g. 0x030d0000 for a 3.13 floor) to gate
# on newer limited-API features if ever needed.
set(FVTK_ABI3_VERSION "0x030c0000" CACHE STRING "fvtk: Py_LIMITED_API value when FVTK_ABI3 is ON (0x030c0000 = CPython 3.12 floor)")

# --- build hygiene -----------------------------------------------------------
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "")

# --- Lever: -O2 for the PR gate only (FVTK_GATE_O2=1) ------------------------
# The shipped (release) wheel is -O3 (CMake's GNU Release default = "-O3 -DNDEBUG"):
# the -O3 mandate stands for tags/releases. But the PR gate does NOT ship; it only
# proves bit-exactness + builds fast. -O2 produces materially faster *codegen* than
# -O3 (it skips -O3's expensive extra passes: aggressive loop unrolling, function
# cloning/IPA-CP at -O3 levels, deeper inlining, vectorizer cost-model churn) while
# remaining BIT-EXACT: without -ffast-math, both -O2 and -O3 honour strict IEEE FP,
# and GCC does NOT reassociate FP / vectorize FP reductions unless -fassociative-math
# (-ffast-math) is set, so the numeric results are identical at maxULP=0. This is the
# SAME safety logic as the existing LTO-off PR gate (LTO changes inlining, not FP
# values). Distinct from -O1 (which the maintainer was wary of): -O2 is the standard
# fully-optimized level, just without -O3's most expensive/least-portable passes.
#
# Gate-only: enabled via the env knob FVTK_GATE_O2=1 (set in the PR-gate
# CIBW_ENVIRONMENT alongside FVTK_LTO=0). Release builds leave it unset -> -O3.
# We override the *_RELEASE flags (not CMAKE_CXX_FLAGS): the per-config Release
# flags are appended AFTER CMAKE_CXX_FLAGS on the command line, so an -O2 added to
# CMAKE_CXX_FLAGS would be overridden by Release's -O3 — the override below makes
# -O2 the effective level.
if ("$ENV{FVTK_GATE_O2}" STREQUAL "1")
  set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
endif ()

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

# === Fast linker + Identical Code Folding (Linux/ELF) ========================
# Two orthogonal levers share one link-line block because BOTH need to name the
# linker (-fuse-ld=…):
#
#   FVTK_FAST_LINKER — which ELF linker drives every link edge. The link phase
#   runs on EVERY build (ccache never caches linking), so on a warm/cached build
#   the link of the ~9 VTK kit .so + the Python .abi3.so dominates. mold and lld
#   are massively-parallel, mmap-based linkers that cut that phase several-fold
#   vs the legacy serial gold/bfd. The linker only ARRANGES already-compiled
#   objects — it does not touch FP codegen — so mold/lld vs gold produce
#   functionally identical, FP-bit-identical filter output and pixel-identical
#   renders (proven by the bitexact + renderexact suites). Values:
#     mold (default) = -fuse-ld=mold   — fastest; dnf-installable on 2_28.
#     lld            = -fuse-ld=lld     — LLVM linker; CI fallback.
#     gold           = -fuse-ld=gold    — legacy binutils gold (prior default).
#     off            = no -fuse-ld      — the compiler's system default (bfd).
#   All of mold/lld/gold implement --icf=all, so ICF composes with any choice.
#
#   FVTK_ICF — Identical Code Folding (--icf=all). Folds byte-for-byte identical
#   functions and ships a single copy: VTK's heavy template instantiation and the
#   generated Python-wrapper boilerplate emit a lot of these. Complements
#   --gc-sections above (which drops UNREACHABLE code); ICF drops code that is
#   reached but DUPLICATED. Measured -10% wheel (47.1 -> 42.2 MiB) on top of the
#   other levers. --icf=all (vs --icf=safe) also folds address-taken functions,
#   so it can break code that relies on two functions having distinct addresses;
#   validated parity-green against PyVista's core+plotting suite. Toggle off with
#   FVTK_ICF=0 for an A/B baseline or if a function-pointer-identity issue
#   surfaces. ICF is a runtime-transparent size lever; the suites prove it stays
#   numerically/pixel exact regardless of which linker folds.
#
# Both read the environment, NOT a cache option: this is a `-C` initial-cache
# file, processed before -D args are applied and before project(), so a
# -DFVTK_*=… cache var would be too late to gate the appends below (and
# CMAKE_SYSTEM_NAME isn't set yet either). $ENV{} is available now and matches
# the repo's env-knob idiom (FVTK_STRIP, FVTK_LTO, FAST).
#
# Gated to gnu (Linux/ELF): -fuse-ld + --icf are ELF-linker features. ld64
# (macOS) has no --icf and a future macOS arm64 build would opt into
# -fuse-ld=lld --icf=all separately once lld is provisioned there; MSVC folds via
# /OPT:ICF in its own block below.
if(_FVTK_TOOLCHAIN STREQUAL "gnu")
  set(_fvtk_link "")
  # --- linker selection (default: mold) ---
  # Resolve a requested name to a -fuse-ld value; "off"/unrecognized means no
  # -fuse-ld (compiler default = bfd). An unset env defaults to mold.
  if (NOT DEFINED ENV{FVTK_FAST_LINKER})
    set(_fvtk_want "mold")
  else ()
    set(_fvtk_want "$ENV{FVTK_FAST_LINKER}")
  endif ()
  if (_fvtk_want STREQUAL "off")
    set(_fvtk_ld "")                      # compiler default linker (bfd)
  elseif (_fvtk_want MATCHES "^(mold|lld|gold)$")
    set(_fvtk_ld "${_fvtk_want}")
  else ()
    message(WARNING "FVTK_FAST_LINKER='${_fvtk_want}' unrecognized "
                    "(want mold|lld|gold|off); using compiler default linker.")
    set(_fvtk_ld "")
  endif ()
  # Graceful fallback: a -fuse-ld=<x> for a linker that isn't installed makes
  # EVERY link fail. mold is the default but is not universally present (CI + the
  # wheel container dnf-install it; a bare dev box may not). If the chosen linker
  # binary is absent, fall back to gold, then to the compiler default (bfd), so
  # the build always links rather than hard-failing on the first link edge.
  if (NOT _fvtk_ld STREQUAL "")
    # mold ships as `mold`/`ld.mold`; lld as `ld.lld`; gold as `ld.gold`.
    if (_fvtk_ld STREQUAL "mold")
      find_program(_fvtk_ld_bin NAMES mold ld.mold)
    else ()
      find_program(_fvtk_ld_bin NAMES "ld.${_fvtk_ld}")
    endif ()
    if (NOT _fvtk_ld_bin)
      find_program(_fvtk_gold_bin NAMES ld.gold)
      if (_fvtk_gold_bin AND NOT _fvtk_ld STREQUAL "gold")
        message(STATUS "FVTK_FAST_LINKER: '${_fvtk_ld}' not found on PATH; "
                       "falling back to gold.")
        set(_fvtk_ld "gold")
      elseif (NOT _fvtk_gold_bin)
        message(STATUS "FVTK_FAST_LINKER: '${_fvtk_ld}' (and gold) not found on "
                       "PATH; using the compiler default linker (bfd).")
        set(_fvtk_ld "")
      endif ()
      unset(_fvtk_gold_bin CACHE)
    endif ()
    unset(_fvtk_ld_bin CACHE)
  endif ()
  if (NOT _fvtk_ld STREQUAL "")
    set(_fvtk_link "${_fvtk_link} -fuse-ld=${_fvtk_ld}")
  endif ()
  unset(_fvtk_want)
  unset(_fvtk_ld)
  # --- ICF (default ON) ---
  if (NOT DEFINED ENV{FVTK_ICF} OR NOT "$ENV{FVTK_ICF}" STREQUAL "0")
    set(_fvtk_link "${_fvtk_link} -Wl,--icf=all")
  endif ()
  if (NOT "${_fvtk_link}" STREQUAL "")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_fvtk_link}" CACHE STRING "" FORCE)
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${_fvtk_link}" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} ${_fvtk_link}" CACHE STRING "" FORCE)
  endif ()
  unset(_fvtk_link)
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
