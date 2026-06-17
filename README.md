# fvtk — fast VTK for PyVista

**fvtk** ("f" for *fast*) is a divergent fork of [VTK](https://gitlab.kitware.com/vtk/vtk)
maintained by the [PyVista](https://github.com/pyvista) organization. It is **not** a
mirror of upstream VTK and it is **not** affiliated with Kitware.

The project has two phases:

1. **Now — a trimmed VTK that installs as its own package.** fvtk contains only the
   modules PyVista imports (core + filters + IO + the full rendering stack) and their
   dependencies. It installs into the **`fvtk/`** import package with dist name **`fvtk`**
   (not `vtkmodules`/`vtk`), so it **coexists** with a stock `vtk` install instead of
   clobbering it. Behavior is identical to stock VTK — the emitted C++ and Python wrappers
   are the same code, only the package name differs — so PyVista runs unchanged against it
   once taught to import `fvtk` (see [Namespace](#namespace--coexists-with-stock-vtk)).
   fvtk ships **two wheels**: a normal **static `cp311`** wheel for Python 3.11, and a single
   **stable-ABI `cp312-abi3`** wheel for Python 3.12+ (installs on CPython 3.12/3.13/3.14+, no
   per-minor matrix). 3.11 cannot be a stable-ABI target (`PyMemberDef` entered the limited API
   only in 3.12), so it gets the static wheel; both are bit-exact with stock VTK 9.6.2 — the abi3
   wheel with one documented exception, wrapped types are heap types so `type(x).__flags__` differs
   in the `HEAPTYPE`/`IMMUTABLETYPE` bits (see the [abi3 lever](#packaging-lever)); the static cp311
   wheel matches byte-for-byte including `__flags__`. `FVTK_ABI3=0` rebuilds the legacy static wheels
   for every version. Smaller wheel, faster build.
2. **Next — swap-for-faster.** Individual VTK components are progressively replaced with
   faster implementations and dead code is stripped, so fvtk diverges from upstream over
   time. The trim is the baseline; the divergence is the point.

This is the result of the **build-trim campaign** — phase 1. The rest of this document is
a handoff guide for continuing development.

---

## Status

| | fvtk (trimmed) | stock `vtk` 9.6.2 |
|---|---|---|
| Fork point | VTK `v9.6.2` (`f49a1dbafa`) | 9.6.2 |
| Wheel size (stripped) | **~37 MB** (36.9 MiB) LTO release · ~30 MB with opt-in PGO | ~120 MB |
| Runtime (PyVista filter bench) | **~2 % faster** than untuned `-O3` (LTO release) · ~26 % with opt-in PGO | reference |
| Modules shipped | ~84 + vendored deps | ~160 |
| Compile units (`ninja` steps) | **~6,900** (wrappers further batched by unity) | ~9,120 |
| Source tree (tracked) | **~140 MB** | ~320 MB |
| Import package / dist name | **`fvtk`** / **`fvtk`** | `vtkmodules` / `vtk` |
| Wheel format | **`cp311` static + single `cp312-abi3`** (3.11 static wheel; 3.12+ one stable-ABI wheel, no per-minor matrix) · `FVTK_ABI3=0` rebuilds the legacy `cp3XX-cp3XX` static wheels | per-minor `cp3XX-cp3XX` |
| Functional validation | **green** — native `import fvtk` smoke (kits, numpy roundtrip, EGL render) + bit-exact to stock VTK 9.6.2 (`maxULP=0`, numpy zero-copy shared+byte-identical) EXCEPT the one documented abi3 `type.__flags__` HEAPTYPE divergence | reference |

---

## What the trim does

Everything below is **API/ABI compatible** — the emitted C++ and Python wrappers are the
same code, just *fewer translation units* and *fewer shipped symbols*. Runtime behavior is
unchanged (verified against the PyVista suite); none of these levers trades runtime
performance, and the build keeps `-O3`.

### Build-size levers

1. **Module deny-list** — `VTK_BUILD_ALL_MODULES OFF`; only PyVista's measured module
   closure is enabled (`fvtk-config/_modules_minimal.cmake`). Source for ~136 never-built
   module directories was removed outright (320 MB → 218 MB).
2. **Lever A — `NOWRAP` (1,173 classes).** Classes PyVista never touches keep their C++
   but skip Python wrapper generation. A single hook in `CMake/vtkModule.cmake` demotes
   `FVTK_NOWRAP_CLASSES` (`fvtk-config/_nowrap_classes.cmake`) from `CLASSES` to
   `NOWRAP_CLASSES`. The drop list is closed under "referenced by a kept class's header" so
   it always builds.
3. **Lever B — `NOCOMPILE` (742 classes).** Classes outside PyVista's reachable closure are
   dropped from the build entirely (no compile, no wrapper, no hierarchy). Hook in
   `CMake/vtkModule.cmake` driven by `fvtk-config/_nocompile_classes.cmake`. The drop set is
   closed under C++ source references, transitive `::New()` factory bases, kept-subclass
   vtable/typeinfo (undefined-at-`dlopen`), and generated `vtk*ObjectFactory.cxx` override
   registrations.
4. **Source pruning** — the source files of the NOCOMPILE classes, plus the
   `Documentation/`, `Examples/`, `.gitlab/` trees and per-module `Testing/` test
   code/data, are removed from the tree (218 MB → ~140 MB). See
   [Gotchas](#gotchas--hard-won-traps) for which files must NOT be deleted.

Levers A + B take the build from ~9,120 to ~6,900 `ninja` steps (−24%) — near the practical
floor for class removal under PyVista parity (PyVista is a near-complete VTK frontend, so
most of what remains is genuinely reachable).

### Compile-time lever

5. **Wrapper unity (`FVTK_WRAP_UNITY`).** Each generated `*Python.cxx` re-parses the same
   `vtkPython*.h` stack (~40 % of its `-O3` cost). A hook in
   `CMake/vtkModuleWrapPython.cmake` batches them into chunked unity translation units
   (default 32 wrappers/TU) that `#include` the per-class files. The wrappers are
   byte-identical. **Measured ~48 % less wrapper-compile CPU at `-O3`**; isolated
   wrapper-phase wall −58 % @ `-j8`, −44 % @ `-j22`. Chunking keeps the phase CPU-bound, so
   the win holds at high core counts (drop the chunk size for bigger CI runners). Unlike
   C++-source unity, generated wrappers have no anonymous-namespace symbol collisions.

6. **Split array-instantiation TUs (`FVTK_SPLIT_BULK_INSTANTIATE`, on by default).** Stock VTK
   concatenates ~17 template-instantiation sources (`vtkAOSDataArrayTemplate`, the implicit
   backends, the generated `vtkType*Array` specializations, etc.) **per numeric type** into one
   `vtkArrayBulkInstantiate_<type>.cxx` (14 of them, one per type in `vtk_numeric_types`). At
   `-O3` each of those bundled TUs is a ~140 s monster, and with only ~14 of them they serialize
   badly on a few-core CI runner — the long pole of the whole build. The bundling was a
   process-spawn optimization that backfires when the per-TU `-O3` cost dwarfs spawn cost.
   `FVTK_SPLIT_BULK_INSTANTIATE` compiles the ~235 component `.cxx` (which already exist on disk;
   the bulk file merely `#include`s them) **directly**, turning the 14 huge TUs into many small
   ones that fill every core evenly. The object code is **byte-identical** (same explicit
   instantiations, same flags, just emitted into separate `.o`), so it is ABI- and
   bit-exact-neutral — it only changes build scheduling. Hooks in `Common/Core/CMakeLists.txt`
   + `Common/Core/vtkTypeArrays.cmake` (the two generation sites that fed the bulk TU);
   `FVTK_SPLIT_BULK_INSTANTIATE=OFF` restores the stock single-bulk-TU-per-type layout.
   **Measured** (CommonCore array-instantiation TUs, `-j4 -O3` no-LTO): wall **361 s → 161 s
   (−55 %, 2.24×)** and total CPU **1210 s → 627 s (−48 %)** — split wins on *both* because GCC's
   per-TU `-O3` cost is super-linear in TU size, so many small TUs are cheaper in aggregate than
   a few huge ones.

7. **Module source unity (`FVTK_SOURCE_UNITY`, on by default).** The wrapper-unity lever (#5)
   batched the *generated* `*Python.cxx`; this is its analogue for the **module C++ source**
   `.cxx` (~2,500 of them). Each re-parses the same heavy VTK/STL header stack
   (`vtkSetGet.h`/`vtkObjectFactory.h`/`vtkDataObject.h` + the dataset headers), the dominant
   cold-compile cost per TU. A hook in `CMake/vtkModule.cmake` turns on CMake's native
   `UNITY_BUILD` (BATCH mode, `FVTK_SOURCE_UNITY_BATCH`=8 by default) per module, concatenating
   several `.cxx` into one TU so that shared parse is amortized once per batch. The emitted
   object code is **byte-identical** (same source, just compiled together) → ABI- and
   bit-exact-neutral; the standing bitexact suite (maxULP=0 vs stock VTK 9.6.2) is the proof.
   Unlike generated wrappers, hand-written VTK sources are *not* uniformly unity-clean, so the
   hook carries three exclusions, all compile-correctness (never numeric): (a) `vtkCommonCore`
   (owned by the array-TU split #6 — batching would undo its parallelism) and `vtkWrappingPythonCore`
   (delicate abi3 `PyType_FromSpec`/static-init runtime) are excluded whole; (b) all `ThirdParty`
   vendored libs (C structs/macros, classic unity poison); (c) ~47 individual hand-written `.cxx`
   with file-local anonymous-namespace globals / `#include`d global-table `.inl` / X11-macro leaks
   that collide by *name* across a batch — listed in `fvtk-config/_source_unity_exclude.cmake`, each
   pulled into its own standalone TU while the rest of its module still batches (generated
   `*Instantiate*.cxx` are auto-excluded by a name pattern). That is <2 % of the source TUs;
   the other ~98 % batch. `FVTK_SOURCE_UNITY=0` disables it for an A/B or to bisect a new breaker.
   **Measured** (cold, no-LTO no-ccache, GCC 14): on the batchable-module subset at `-j4` (the CI
   parallelism, isolating the lever from the unaffected `CommonCore` cluster) wall **165 s → 92 s
   (−44 %)** with the compiled object-TU count **782 → 135 (−83 %)**. On the *full* wheel at `-j32`
   wall **239 s → 194 s (−18.8 %)**, TU count **3,614 → 2,186 (−40 %)** — the full-wheel delta is
   smaller because the `CommonCore` `-O3` array-instantiation cluster (lever #6's domain, excluded
   from unity) is a serial long pole identical in both legs that dilutes the *total*; the per-TU
   parse-amortization win is the −44 % subset figure. **GCC<12-gated** (see #5): inert on the
   current manylinux2014 (GCC 10.2.1) CI container; bump the cibuildwheel image to manylinux_2_28
   (GCC 12+) to activate it — and the wrapper-unity lever #5 — in CI.

### Binary-size levers (compiled C++)

7. **SOA array-dispatch OFF** (`VTK_DISPATCH_SOA_ARRAYS` + `VTK_DISPATCH_SCALED_SOA_ARRAYS`
   = `OFF`, reverting to VTK's own default). VTK's `vtkArrayDispatch` instantiates a templated
   fast-path for each array *layout* × value type at every filter call site. With SOA +
   ScaledSOA on, the type list is `AOS + SOA + ScaledSOA` × ~14 types ≈ 42 instantiations per
   site (and `Dispatch2`/`Dispatch3` multiply it N²/N³); AOS-only is ~14. PyVista only ever
   constructs **AOS** arrays (`numpy_to_vtk` → `vtkFloatArray` etc.), never SOA — so the SOA
   fast-paths are dead weight. Off = ~3× less generated dispatch code in the big Filters/Common
   kits, no behavior change (an SOA array, if one ever appeared, still works via the virtual
   `vtkDataArray` fallback). **This is the single biggest binary lever.** (The SOA array
   *classes* themselves stay — they're woven into CommonCore's array system; only their
   dispatch fast-paths are dropped.)

8. **Link-time dead-code elimination** (`-ffunction-sections -fdata-sections` +
   `-Wl,--gc-sections`). Emits each function/datum in its own section and lets the linker drop
   the unreachable ones. Safe with VTK's `-fvisibility=hidden` (only exported symbols are GC
   roots; factory/virtual/wrapper paths all go through exports). Removes real code and stacks
   on top of strip.

Levers 7 + 8 together take the stripped wheel **65 MB → 49 MB (−24%)** with the PyVista suite
green (9,731 passed / 8 pre-existing env-fails / 0 introduced).

9. **Identical Code Folding (`FVTK_ICF`, on by default).** `-fuse-ld=gold -Wl,--icf=all` folds
   functions whose machine code is byte-for-byte identical and ships a single copy. VTK's heavy
   template instantiation (each instantiation is a separate emitted copy, identical once past the
   type system) and the generated Python-wrapper boilerplate produce a lot of these. Complements
   lever 8: `--gc-sections` drops code nothing *reaches*; ICF drops code that is reached but
   *duplicated* — they stack. `--icf=all` (vs `--icf=safe`) also folds address-taken functions, so
   it can break code relying on two functions having distinct addresses; VTK's factory/callback
   machinery stores/compares function pointers, so this was the correctness risk. **Validated
   parity-green:** a differential PyVista core+plotting run (2,104 tests) gave byte-identical
   outcomes with vs without ICF (0 regressions), plus the native `smoke-fvtk.py` factory/EGL path.
   **Measured −10% wheel (47.1 → 42.2 MiB)**; the fold concentrates in `libvtkCommon` (−19.8%).
   Disable with `FVTK_ICF=0 ./build-fvtk.sh` for an A/B baseline (gold `--icf=safe` is the
   strictly-weaker fallback). Linux/gold only; needs `binutils` (in `shell.nix`).

10. **Wrapper TU size-opt (`FVTK_WRAP_OPTSIZE`, on by default).** The generated `*Python.cxx`
    wrappers are argument-marshalling shims, not hot code, yet they inherit the project `-O3`.
    A hook in `CMake/vtkModuleWrapPython.cmake` appends `-Oz` to the wrapper (unity) TUs only —
    last `-O` wins, so it overrides `-O3` for those TUs and nothing else. **~1.2 MiB off the
    wheel** at zero runtime cost (cold code). `FVTK_WRAP_OPTSIZE=0` to disable.

11. **Array-dispatch value-type trim (`FVTK_DISPATCH_MINIMAL`, on by default).** VTK's default
    `vtkArrayDispatch` typelist is ~14 value types; PyVista's `numpy_to_vtk` overwhelmingly yields
    double/float, plus `vtkIdType` (ids/connectivity) and uint8 (RGBA colors). A hook in
    `Common/Core/vtkCreateArrayDispatchArrayList.cmake` trims the AOS + StructuredPoint dispatch
    lists to those **4** (`double;float;vtkIdType;unsigned char`; `vtkIdType` is 64-bit signed so
    int64 still dispatches through it), so `Dispatch`/`Dispatch2`/`Dispatch3` instantiate far fewer
    workers across every dispatched filter TU in the Filters/Common kits — and because Dispatch2/3
    multiply the list N²/N³, 14 → 4 collapses the cross-filter fan-out hard. **Bit-exact
    (maxULP=0):** an excluded value type (int32, narrow ints) still works via the virtual
    `vtkDataArray` fallback (same mechanism as the SOA-off lever) — only the typed fast path is
    dropped, never a result; the only cost is *runtime* speed for int32/narrow-int workloads.
    `FVTK_DISPATCH_MINIMAL=0` restores the full list. (This trims the *dispatch* typelist only — the
    per-type array *classes* and their bulk instantiations stay, because CommonCore's own array
    machinery references the SOA/implicit backends regardless of the dispatch switches.)

11b. **Drop dead implicit-array families (`FVTK_DROP_DEAD_ARRAYS`, on by default).** The
    `vtkStridedArray` and `vtkStdFunctionArray` implicit families are never constructed by PyVista,
    and their dispatch options default OFF (already absent from the dispatch typelist). This drops
    their per-numeric-type instantiation TUs (`vtkStridedArrayInstantiate_*`,
    `vtkStdFunctionArrayInstantiate_*`, `vtkStridedImplicitBackendInstantiate_*`) and generated
    fixed-size specialization classes from the build. The template headers stay in place (header-only,
    zero cost), so it is a pure instantiation drop, trivially reversible via `FVTK_DROP_DEAD_ARRAYS=0`.
    Unlike SOA/ScaledSOA (load-bearing), these two have no reference from any kept CommonCore
    machinery, so the cut is link-safe.

Levers 9–11 plus a strip-coverage fix (the kit `libvtkCommon.so` was shipping unstripped — its
26 MB `.symtab` is now removed via a wheel re-strip+repack) take the stripped wheel **49 → 38 MiB**.
Validated parity-green: differential PyVista core+plotting (2,104 tests), byte-identical outcomes,
0 regressions.

### Runtime-speed levers (compiled C++)

The campaign's second axis: make everything PyVista *calls* fast, without dropping or
slowing any module. These never trade runtime for size — they make the code faster, and
LTO shrinks it as a side effect. ISA floor is left at **baseline x86-64** (no `-march`) for
maximum wheel portability.

12. **LTO (`FVTK_LTO`, on by default).** `-flto=auto` (GCC parallel-WPA — the GCC analogue of
    Clang ThinLTO; streams whole-program analysis across cores so it's ~30–40 min cold, not
    serial-LTO hours). Cross-TU inlining + devirtualization; the link-line carries `-O3` so LTO
    optimizes for speed. Composes with gold `--icf=all` (ICF folds the post-LTO objects) and
    `--gc-sections`. **Measured +~2 % overall on the PyVista filter/render benchmark** (4–5 % on
    compute-bound kernels — clip/glyph/smooth/contour; flat on GL-render and bandwidth-bound
    paths that `-O3` already saturates) **and −3 % wheel** as a side effect. `FVTK_LTO=0` for
    fast iteration builds (~13 min cold).

13. **`-fno-semantic-interposition` (`FVTK_SEMINTERP`, on by default).** By default a shared
    library must assume any exported symbol could be interposed at load time (`LD_PRELOAD`), so
    GCC routes those calls through the PLT and can't inline/devirtualize across them. A
    self-contained viz wheel is never interposed in its own internals, so this is safe and lets
    the compiler inline within each `.so` — a win for VTK's virtual-dispatch + template hot
    paths (CPython itself ships this way). Free: no portability or build-time cost.

14. **Profile-Guided Optimization (`FVTK_PGO=gen|use`, opt-in — NOT the default release).**
    The campaign's biggest *raw* lever, but **off for the shipped wheel**: its +26%/−25% is
    concentrated on a curated filter-training workload (`tools/pgo-train.py`) and it ships the
    ~96 %-cold remainder at `-Os`, so untrained workloads see little gain and a bigger-than-`-O3`
    penalty on cold paths. The shipped release is **LTO-only** (lever 12); PGO is kept as an
    opt-in build (`ci/pgo-build.sh`) for users who want to re-tune it to their own workload.
    A three-phase build (`ci/pgo-build.sh`): (1) **instrument** —
    `-fprofile-generate` (atomic counters; VTK's SMPTools run threaded); (2) **train** —
    run `tools/pgo-train.py` (a balanced, representative sweep of the PyVista filter hot
    paths) against the instrumented wheel so real branch/call frequencies land in `.gcda`;
    (3) **rebuild** — `-fprofile-use` + LTO + ICF, now guiding inlining, hot/cold splitting
    and branch layout from the measured profile. gen and use share the build dir (`.gcda`
    are keyed to object paths); `CMakeCache.txt` is dropped between phases so the use-config
    starts from clean flags. The training is deliberately **GL-free** — every PGO win is in
    CPU-bound filters, and a render segfault on a headless box would bypass gcov's atexit dump
    and lose the whole profile. **Measured +26% on the PyVista filter benchmark vs untuned
    `-O3`** (`slice` 2.9×, `clip` 2.2×, `threshold` 1.9×; contour neutral; GL-bound render
    flat) **and −25% wheel** (28.5 MiB — PGO splits cold paths out and treats them for size).
    ~3× build time on top of LTO, so it runs only in the release job, not per-push.

15. **Multicore-by-default for bit-exact-safe filters (on by default; overridable).** fvtk
    keeps the *global* `vtkSMPTools` backend at **Sequential**, so the whole library is serial
    and byte-for-byte identical to stock VTK 9.6.2 out of the box. A small, audited set of
    filters whose parallel loops are *provably* bit-exact under any thread count — each
    iteration writes only its own pre-sized output slot, `out[i] = f(in[i])`, with no
    `InsertNext*` append, no floating-point reduction, no order-dependent locator insert —
    opt **into** multithreading locally, defaulting to a cap of **4 threads**. The enabled set
    is `vtkWarpVector`, `vtkWarpScalar`, `vtkPolyDataNormals` (cell + point normals), and
    `vtkElevationFilter`. The mechanism is a tiny CommonCore helper
    (`fvtk::RunSafeFilterParallel`, `Common/Core/vtkFVTKSMPDefaults.{h,cxx}`) that wraps just
    those filters' `vtkSMPTools::For` regions in `vtkSMPTools::LocalScope`, temporarily
    activating the (already-compiled-in) STDThread backend and then restoring the global
    Sequential state — so nothing else in the library is threaded, and every other filter stays
    serial/bit-exact. **All `tests/bitexact/` cases stay maxULP = 0 at the default config**, and
    the enabled filters are byte-identical at 1, 4 and 8 threads (determinism proof). Overrides
    use the *existing* VTK SMP APIs and take precedence over the default:

    | Goal | How |
    |---|---|
    | Raise / lower the cap | `VTK_SMP_MAX_THREADS=8` (or any N); honored, not re-capped to 4 |
    | Force everything serial | `VTK_SMP_MAX_THREADS=1`, or `FVTK_SMP_DEFAULT=0` |
    | Explicit programmatic count | `vtkSMPTools.Initialize(n)` |
    | Thread the **whole** library (not bit-exact) | `VTK_SMP_BACKEND_IN_USE=STDThread`, or `vtkSMPTools.SetBackend("STDThread")` |

    Precedence (first match wins): `FVTK_SMP_DEFAULT=0` → global backend already non-Sequential
    (inherit it) → `Initialize(n)` → `VTK_SMP_MAX_THREADS` env → default STDThread @ 4. The
    helper also refuses to switch the (process-global, not-thread-safe) SMP singleton while
    already inside a parallel scope (`vtkSMPTools::IsParallelScope()`), inheriting the caller's
    backend instead. We set **defaults**, not removing knobs.

16. **AVX2 SIMD on the hot vertical kernels via function-multi-versioning (single portable
    wheel; no `-march`).** The compute-bound, element-wise (`out[i] = f(in[i])`) inner loops of
    `vtkLinearTransform` (the 4×4 matrix·point math behind `vtkTransformFilter` /
    `vtkTransformPolyDataFilter`) and `vtkWarpVector` are hoisted into dedicated free functions
    marked `__attribute__((target_clones("default","avx2")))`. GCC emits **two clones per
    kernel** — a baseline **SSE2** `.default` clone and a wide-SIMD `.avx2` clone — plus an
    **IFUNC resolver** that picks the right one at load time from CPUID. So a *single* wheel,
    still built at the baseline x86-64 ISA floor (no `-march` bump, no lost portability for
    pre-Haswell CPUs), runs 256-bit AVX2 where the CPU has it and the correct SSE2 path where it
    doesn't. **Bit-exactness is preserved** (maxULP = 0 vs stock VTK 9.6.2): the two FMV'd TUs
    are compiled with `-ffp-contract=off` (`set_source_files_properties`) so neither clone
    contracts the `a*b+c`-shaped matrix/warp math into a fused `vfmadd` — FMA contraction is the
    one transform that would diverge from stock by 1 ULP on adversarial data. Verified: the
    `.avx2` clones carry **0 `vfmadd`** and use `ymm`; the whole `tests/bitexact/` suite (incl. a
    new `transform` op) stays maxULP = 0; and a **2 M-point, double-precision, adversarial
    (wide-dynamic-range) warp + transform** — inputs chosen to *expose* any FMA double-rounding —
    is **byte-identical** to stock VTK 9.6.2 on both the AVX2 and the forced-SSE2 (default-clone)
    paths. Measured (i9-14900K, 1 thread, AVX2 vs forced-SSE2 default clone): **transform 1.40×
    cache-resident / ~1.05× DRAM** (compute-bound, win everywhere), **warp 1.41× cache-resident /
    ~flat DRAM** (bandwidth-bound — wider lanes help only when the working set fits in cache).
    Composes with lever 15: a 2 M-point warp runs **~3.0× over serial-SSE2** at 4 threads (the
    SMP threading carries that win; SIMD is ~flat at DRAM scale, where memory bandwidth, not
    arithmetic, is the bottleneck). Higher-arithmetic-intensity vertical kernels (the transform)
    are where AVX2 pays; bandwidth-bound warps gain only cache-resident — and reduction kernels
    (contour/smooth/decimate accumulation) are deliberately **not** FMV'd (the compiler can't
    reorder a sum without `-ffast-math`, which is forbidden).

Levers 12–14 are validated parity-green: differential PyVista core+plotting (**4,088 tests**),
**0 regressions** vs the untuned `-O3` build (identical pass/fail/skip outcomes).

### Packaging lever

15. **Stable ABI / abi3 (`FVTK_ABI3`, DEFAULT ON; the cibuildwheel backend forces it OFF on
    cp311).** The Python wrapper runtime and the wrapper code generator are ported to
    `PyType_FromSpec` heap types behind `Py_LIMITED_API`, so the wrappers compile against the
    CPython **limited API** (floor `cp312` = `Py_LIMITED_API 0x030c0000`) and the build emits a
    **single `cp312-abi3` wheel** that installs and imports on **CPython 3.12+** — including
    future minors with **no rebuild**. The floor is **3.12, not 3.11**: `PyMemberDef` and the
    `Py_T_*`/`Py_READONLY` member constants the heap-type wrappers emit only entered the stable
    ABI in 3.12 ([gh-93274](https://github.com/python/cpython/issues/93274)), so 3.11 cannot be a
    stable-ABI target. **fvtk therefore ships TWO wheels:** a normal **static `cp311`** wheel
    (built with `FVTK_ABI3=0` on the cp311 leg) for Python 3.11, and the single **`cp312-abi3`**
    wheel for 3.12+. This collapses the per-minor `cp312 cp313 cp314` part of the matrix to one
    leg: the wrappers (≈85 modules / ~1,664 generated TUs) compile **once** for 3.12+ instead of
    per minor (cibuildwheel's abi3 dedup reuses the wheel for cp313/cp314 — exactly two builds:
    cp311 static + cp312 abi3), plus zero-cost support for every future CPython minor.

    **Bit-exactness.** The abi3 wheel is byte-for-byte identical to stock VTK 9.6.2 on every
    numeric and behavioral fact — `maxULP=0` across the bitexact suite, **numpy zero-copy
    buffer shared + byte-identical**, identity/`isinstance`/mro/`repr`/weakref/instance-`__dict__`
    all matching — **except one documented divergence**: `type(x).__flags__`. Every limited-API
    type is necessarily a *heap* type (`PyType_FromSpec` is the only way to make a type under
    the limited API), so `Py_TPFLAGS_HEAPTYPE` (`1<<9`) is set and `IMMUTABLETYPE` (`1<<8`)
    is cleared on every wrapped class, vs static types on stock. This is intrinsic to the
    stable ABI, not a fixable shim gap; the wrapper-parity gate **expects** exactly this flip
    (and the `reference` helper's `BASETYPE` bit) and flags any other divergence.

    The **static cp311 wheel** is strict byte-for-byte parity *including* `__flags__` (it is a
    normal static-type build); only the abi3 wheel carries the `__flags__` divergence above.

    **Escape hatch.** Build with `-DFVTK_ABI3=OFF` (or `FVTK_ABI3=0` in the wheel backend env)
    to produce the **legacy per-version static-type wheels** (`cp3XX-cp3XX`, strict
    byte-for-byte parity *including* `__flags__`) for **every** version (not just 3.11). The
    shipped matrix already builds cp311 this way. See `docs/abi3-feasibility.md`.

### Wheel-size lever

6. **Symbol strip (`FVTK_STRIP=1`).** `strip --strip-all` on every shipped `.so` removes
   the static symbol table (`.symtab`/`.strtab`, ~40 % of a Release lib — e.g. `libvtkCommon`
   178 → 107 MB) while keeping `.dynsym` (runtime-safe; matches stock manylinux wheels). The
   strip walks the wheel-staging tree, so all 142 shipped libs (incl. the kit libraries) are
   stripped, not just the SDK wrappers. **Measured: with levers 7+8, stripped wheel = 49 MB
   (47 MiB) — ~60 % smaller than the stock `vtk` wheel (~120 MB)** (strip alone, without 7+8,
   was 65 MB). The wheel is already maximally deflate-compressed; re-zipping does not shrink it.

> **Not used: auditwheel.** `auditwheel` is a *portability/tagging* tool, not a size tool —
> measured `repair` on our wheel was +620 bytes (it only retags `linux_x86_64` →
> `manylinux_2_39_x86_64`; VTK `dlopen`s GL so there's nothing to vendor). It will be needed
> at the CI/distribution stage to produce a PyPI-acceptable `manylinux` tag, but it does not
> shrink the wheel. (LTO — lever 12 — is primarily a *runtime* lever, but cross-TU dead-code
> elimination also shrinks the wheel ~3 %; it costs ~3× build time, so `FVTK_LTO=0` for fast
> iteration.)

---

## Building

The build self-execs into a `nix-shell` (`shell.nix`) that provides the GL/EGL/OSMesa/X11
stack, pins `cmake` 4.1.2, and uses Python 3.13 + `ccache`.

```bash
# Fast iteration wheel (LTO off, stripped) — the per-push smoke build:
FVTK_LTO=0 FVTK_STRIP=1 ./build-fvtk.sh

# Release wheel (LTO + ICF + strip) — the shipped default, ~+2% / ~37 MiB:
FVTK_LTO=1 FVTK_ICF=1 FVTK_STRIP=1 ./build-fvtk.sh

# Opt-in: profile-guided build (instrument → train → rebuild), ~26% faster +
# ~25% smaller but ~3× build time and tuned to a curated filter workload. NOT the
# shipped default; clones pyvista automatically (or set PYVISTA_DIR to a checkout):
./ci/pgo-build.sh
```

Knobs (environment variables):

| var | default | meaning |
|---|---|---|
| `PROFILE` | `minimal` | `minimal` (PyVista closure) · `fast`/`linux` (all modules) |
| `FAST` | `1` | `0` enables LTO (production) |
| `FVTK_STRIP` | `0` | `1` strips shipped `.so` symbol tables |
| `FVTK_ICF` | `1` | `0` disables gold ICF (link-time identical-code folding); minimal profile only |
| `FVTK_WRAP_OPTSIZE` | `1` | `0` disables `-Oz` on the Python wrapper TUs |
| `FVTK_DISPATCH_MINIMAL` | `1` | `0` restores the full ~14-type array-dispatch list (vs PyVista's 6) |
| `FVTK_LTO` | `1` | `0` disables LTO (`-flto=auto`); minimal profile. Off ≈ ~13 min cold vs ~3× with LTO |
| `FVTK_SEMINTERP` | `1` | `0` restores default semantic interposition (disables intra-`.so` inlining) |
| `FVTK_PGO` | _(unset)_ | `gen`/`use` phases of profile-guided optimization; orchestrated by `ci/pgo-build.sh` |
| `BUILD` | `./build-fvtk` | build directory |
| `BUILD_JOBS` | `8` | parallel compile jobs |
| `USE_CCACHE` | `1` | compiler launcher via `ccache` |

The wheel lands in `<BUILD>/dist/*.whl`. Cold `-j8` build is ~29 min; warm (ccache)
rebuilds are minutes. **Always verify a deletion or config change in a *fresh* `BUILD` dir**
— a dirty incremental cache can hide a `configure`/generate break (see Gotchas).

---

## Repository layout

```
fvtk-config/            # all fvtk-specific build policy (the only "our code" in CMake terms)
  minimal.cmake         #   the default profile: deny-by-default + production knobs
  _modules_minimal.cmake#   the enabled-module closure (PyVista's measured set)
  _nowrap_classes.cmake #   Lever A drop list  (1,173 names) — skip Python wrapper, keep C++
  _nocompile_classes.cmake# Lever B drop list  (742 names)  — drop from build entirely
  fast.cmake / linux.cmake / macos.cmake / windows.cmake   # all-module profiles
build-fvtk.sh           # the build driver (nix re-exec, cmake configure, build, strip, wheel)
shell.nix               # GL/EGL/OSMesa/X11 + toolchain for the build
CMake/vtkModule.cmake            # hosts the NOWRAP + NOCOMPILE hooks (search "FVTK_")
CMake/vtkModuleWrapPython.cmake  # hosts the wrapper-unity hook (search "FVTK_WRAP_UNITY")
<Area>/<Module>/        # VTK source, trimmed (e.g. Common/Core, Filters/General, ...)
```

Everything outside `fvtk-config/`, `build-fvtk.sh`, `shell.nix`, and the three hook edits in
`CMake/` is upstream VTK source. The hooks are **inert until the lists are defined**, so the
tree still builds as stock VTK with a different `-C` cache file.

### Branches (on `pyvista/fvtk`, private)

| branch | purpose |
|---|---|
| `main` | the published trimmed fork — what `git clone` checks out |
| `feat/build-trim` | the build-trim campaign branch (this work) |
| `feat/fvtk-namespace` | original `vtkmodules → fvtk` rename spike (now **merged into `main`**) |
| `feat/ci` | early GitHub Actions wheel-matrix scaffolding (paused) |

---

## How the trim works (extending it)

All three levers are name-driven lists consumed by hooks; adding or restoring a class is a
one-line edit, no C++ changes.

- **To keep a class's Python wrapper** (undo a NOWRAP): delete its line from
  `_nowrap_classes.cmake`.
- **To compile a class again** (undo a NOCOMPILE): delete its line from
  `_nocompile_classes.cmake`.
- **To drop a new class**: add its name to a list. NOWRAP is zero-risk (C++ still compiles);
  NOCOMPILE needs closure analysis (the build is the oracle — `configure → build -k 0 →
  import-smoke`; undefined `::New()`/typeinfo symbols only surface at `dlopen`, not at link).
- **Wrapper-unity chunk size**: `FVTK_WRAP_UNITY_CHUNK` in `minimal.cmake` (must be a `CACHE`
  var to reach function scope). Smaller chunks parallelize better on big runners.

---

## Parity & validation

The gate is **PyVista's own test suite**, not a synthetic module-closure check: build the
fvtk wheel, install PyVista against it, run `tests/core` + `tests/plotting` off-screen
(EGL). Bar: **zero new failures versus stock `vtk` 9.6.2**.

Latest run: **9,731 passed / 8 failed**, where all 8 reproduce identically on a stock `vtk`
9.6.2 environment (missing optional `trame`, an image-cache cubemap test, a post-9.6.2
`VTKImplicitArray` feature, `test_tinypages` sphinx-env). **0 failures introduced by the
trim.**

How it's run:
- PyVista source: `/home/alex/source/pyvista` (already adapted to test against a custom
  wheel — **do not modify it**).
- A clean venv installs PyVista + test deps, force-installs the fvtk wheel, runs
  `pytest -n 8 --dist worksteal` off-screen.
- A parallel stock-`vtk` venv reproduces any suspected failure node-ID for apples-to-apples
  attribution — that's how the 8 env-fails were proven pre-existing.

Because source pruning does not change which classes compile (the NOCOMPILE list already
excluded them), a green configure + compile + import-smoke is sufficient proof a pruning
step is safe — the resulting wheel is functionally identical.

---

## Gotchas / hard-won traps

These cost real time during the campaign; read before extending.

1. **Not every `Testing/` dir is deletable.** `Testing/Core`, `Testing/DataModel`,
   `Testing/GenericBridge`, `Testing/IOSQL`, `Testing/Rendering`, `Testing/Serialization`
   carry a `vtk.module` and are referenced by VTK's top-level reject logic *even with
   `VTK_BUILD_TESTING OFF`* — deleting them gives "modules requested or required, but not
   found". Delete only per-module test *content* (`*/Testing/Cxx`, `Python`, `Data`), never a
   `Testing/*` directory that contains a `vtk.module`.
2. **NOCOMPILE only filters `CLASSES`.** A handful of classes are also listed in a module's
   explicit `SOURCES`/`TEMPLATES` (e.g. `vtkJoinTables.txx`, `vtkPolynomialSolversUnivariate`,
   `vtkExprTkFunctionParser`, `vtkThreadedCallbackQueue`) — those still compile, so their
   source files must NOT be deleted. Rule: never delete a file whose exact name appears in any
   `CMakeLists.txt`.
3. **Structurally-required disabled modules.** VTK's top-level `CMakeLists.txt`
   unconditionally references `VTK::mpi`, `VTK::catalyst`, `RenderingWebGPU`, Java,
   SerializationManager in its reject list; their definition dirs (`Utilities/MPI`,
   `Utilities/Catalyst`, `Rendering/WebGPU`, `Utilities/Java`, `Serialization/Manager`) must
   stay even though they never compile.
4. **WANT-silent-drop cascade.** Forcing a module to `NO` that is a transitive dependency of a
   loaded rendering module silently removes the whole rendering stack while configure still
   "succeeds". Classify removals by runtime-trace **and** dependency-closure; pyvista-loaded
   modules are pinned `YES` (loud-fail) in `_modules_minimal.cmake`.
5. **Prove deletions in a fresh build dir.** A dirty incremental cache can mask a generate
   break; the first "passing" build after a deletion may just be reusing stale state.
6. **nix shell python pinning.** `shell.nix` must provide Python 3.13; VTK derives the wheel
   ABI suffix from the first `python3` on `PATH`. `build-fvtk.sh` also installs a
   `.pyshim313` belt-and-suspenders.
7. **cmake pin.** nix `cmake` 4.1.2 must be first on `PATH`; a pip `cmake` 4.2.x regresses
   nested `try_compile`. `build-fvtk.sh` handles this.
8. **C++-source unity is hostile in VTK** (anonymous-namespace symbol collisions across
   `.cxx`). Only *wrapper* unity is safe. Don't retry C++ unity without an offline
   collision-offender list.

---

## Namespace — coexists with stock VTK

fvtk installs into the **`fvtk/`** import package (not `vtkmodules/`) with dist name
**`fvtk`** (not `vtk`). Stock VTK and fvtk therefore occupy different pip slots and
different import namespaces, so `pip install fvtk` no longer clobbers — or gets clobbered
by — an existing `vtk` install. `import fvtk` / `from fvtk.vtkCommonCore import vtkPoints`
work exactly like the `vtkmodules` equivalents.

**Mechanism** (all on `main`, originally spiked on `feat/fvtk-namespace`):

- `vtk_module_wrap_python(... PYTHON_PACKAGE "fvtk" ...)` and the `Wrapping/Python/fvtk/`
  source tree (the former `vtkmodules/` — `util/`, `numpy_interface/`, `gtk/qt/tk/wx/`,
  `__init__.py.in`, `all.py.in`).
- `CMake/setup.py.in` sets `dist_name = 'fvtk'`; `VTK_DIST_NAME_SUFFIX` stays empty.
- The Python C wrapper machinery references the package by name —
  `Wrapping/PythonCore/PyVTK*.cxx`, `Wrapping/Tools/vtkWrapPython*.c`, and
  `vtkPythonAppInit.cxx` — all updated `vtkmodules` → `fvtk`.
- `build-fvtk.sh` strips/prunes under `$BUILD/fvtk` and rewrites the generated
  `setup.py` package list.

**Validation.** Because the rename changes only the *package name* — the emitted C++ and
Python wrappers are byte-identical to the `vtkmodules` build that passed the full PyVista
suite — the gate is a native smoke test rather than the stock-pyvista suite (which imports
`vtkmodules` and so cannot drive fvtk unpatched). See `smoke-fvtk.py`: `import fvtk`
with no stray `vtkmodules`, the core kits, a `numpy_support` roundtrip, a filter pipeline,
and an EGL offscreen render — all green.

**Using it from PyVista.** PyVista imports `vtkmodules`, so it must be taught about `fvtk`.
Two paths:

1. *Quick shim* (no PyVista changes) — install a `sys.meta_path` finder that redirects
   `vtkmodules[.*]` to `fvtk[.*]` **before** importing pyvista:

   ```python
   import importlib.util, sys, fvtk
   class _FvtkShim:
       def find_spec(self, name, path=None, target=None):
           if name == "vtkmodules" or name.startswith("vtkmodules."):
               return importlib.util.find_spec("fvtk" + name[len("vtkmodules"):])
           return None
   sys.meta_path.insert(0, _FvtkShim())
   import pyvista  # now resolves vtkmodules.* against fvtk.*
   ```

   This is the throwaway-validation path (patch a *copy* of PyVista or use the shim — do
   not edit a checked-out PyVista source in place).

2. *Proper support* — a PyVista backend selector that imports `fvtk` directly (e.g. a
   `PYVISTA_VTK_BACKEND=fvtk` env switch). This is the real downstream change and the
   intended end state once fvtk lands in the PyVista org.

---

## Roadmap

- **CI** — wheel matrix mirroring PyVista's support set (Python 3.10–3.14 ×
  Linux-x86_64 / macOS-arm64 / Windows-x86_64). Build inside a `manylinux` image to lower the
  glibc floor, then `auditwheel repair` for the `manylinux` tag (portability, not size).
  Mine VTK's own `.gitlab/os-linux.yml` wheel jobs for the OSMesa/EGL handling — stock VTK
  wheels are not trivial cibuildwheel.
- **PyVista-side import shim** — fvtk now installs as `fvtk` (not `vtkmodules`), so PyVista
  must be taught to import it. See [Namespace](#namespace--coexists-with-stock-vtk) for the
  one-line shim approach (`sys.modules` alias) versus a proper PyVista backend selector.
- **Swap-for-faster** — replace hot VTK components with faster implementations. This is the
  divergence phase where fvtk earns the "f"; the trim is just the baseline.

---

## License

VTK is distributed under the OSI-approved BSD 3-clause License; see
[`Copyright.txt`](Copyright.txt). fvtk inherits that license. Upstream provenance (VTK
`v9.6.2`) is recorded in the root commit. For the original VTK project, see
[vtk.org](https://www.vtk.org/).
