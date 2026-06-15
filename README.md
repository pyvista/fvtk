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
   Smaller wheel, faster build.
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
| Wheel size (stripped) | **~30 MB** (28.5 MiB) PGO release · ~37 MB LTO-only | ~120 MB |
| Runtime (PyVista filter bench) | **~26 % faster** than untuned `-O3` (PGO release) · ~2 % LTO-only | reference |
| Modules shipped | ~84 + vendored deps | ~160 |
| Compile units (`ninja` steps) | **~6,900** (wrappers further batched by unity) | ~9,120 |
| Source tree (tracked) | **~140 MB** | ~320 MB |
| Import package / dist name | **`fvtk`** / **`fvtk`** | `vtkmodules` / `vtk` |
| Functional validation | **green** — native `import fvtk` smoke (kits, numpy roundtrip, EGL render) + byte-identical to the parity-green `vtkmodules` build | reference |

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
    `vtkArrayDispatch` typelist is ~14 value types; PyVista's `numpy_to_vtk` only ever yields
    float/double/int/uint8/int64. A hook in `Common/Core/vtkCreateArrayDispatchArrayList.cmake`
    trims the AOS + StructuredPoint dispatch lists to those 6, so `Dispatch`/`Dispatch2`/`Dispatch3`
    instantiate far fewer workers in the Filters/Common kits. **Correctness-preserving:** an
    excluded value type still works via the virtual `vtkDataArray` fallback (same mechanism as the
    SOA-off lever) — only the fast path is dropped, never a result. `FVTK_DISPATCH_MINIMAL=0`
    restores the full list. (Note: this trims the *dispatch* typelist only — the per-type array
    *classes* and their bulk instantiations stay, because CommonCore's own array machinery
    references the SOA/implicit backends regardless of the dispatch switches.)

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

14. **Profile-Guided Optimization (`FVTK_PGO=gen|use`, release builds).** The campaign's
    biggest lever. A three-phase build (`ci/pgo-build.sh`): (1) **instrument** —
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

Levers 12–14 are validated parity-green: differential PyVista core+plotting (**4,088 tests**),
**0 regressions** vs the untuned `-O3` build (identical pass/fail/skip outcomes).

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

# Default wheel (LTO + ICF + strip):
FVTK_STRIP=1 ./build-fvtk.sh

# Release wheel — full profile-guided build (instrument → train → rebuild),
# ~26% faster + ~25% smaller, ~3× build time. Trains on PyVista's filter hot
# paths; clones pyvista automatically (or set PYVISTA_DIR to an existing checkout):
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
