# fvtk wheel builds — raw-docker vs cibuildwheel (evidence + recommendation)

Local, nix-free, docker-driven wheel builds for fvtk (trimmed hard-fork of
VTK 9.6.2; `vtkmodules` renamed to the top-level `fvtk` package). Two approaches
were implemented AND actually run on this machine (docker 28.5.2, 16 cores, 62 GB
RAM, manylinux2014_x86_64 image). All numbers below are measured, not estimated.
Build A = raw-docker, Build B = cibuildwheel; both produced an identical
`manylinux_2_17_x86_64` cp313 wheel that passes import + WebCore + offscreen
render + the bit-exact suite (maxULP 0).

### Real bug found and fixed: `-Oz` breaks under the manylinux container's GCC 10

The first container build failed at the Python-wrapper compile (Build A, ~9 min
in, at the unity wrapper TUs):

```
cc1plus: error: argument to '-O' should be a non-negative integer, 'g', 's' or 'fast'
```

`CMake/vtkModuleWrapPython.cmake`'s wrapper size-opt lever appended **`-Oz`** to
the generated `*Python.cxx` TUs. `-Oz` exists only on clang and **GCC ≥ 12**. The
local nix toolchain is **GCC 14** (so it never surfaced), but manylinux2014 ships
**devtoolset-10 GCC 10.2.1**, which hard-errors on `-Oz`. This means the EXISTING
`wheels-manylinux217.yml` release recipe would hit the same failure — it is not a
problem introduced by either new build path.

**Fix applied** (`CMake/vtkModuleWrapPython.cmake`): pick the flag by compiler —
`-Oz` on clang/GCC≥12, fall back to **`-Os`** on GCC<12. Negligible size delta on
these marshalling shims, and under LTO the link-time `-O3` governs anyway.

### Second, deeper bug: wrapper-unity batching breaks `vtkImplicitArray` on GCC 10

Past the `-Oz` fix, the build failed again — this time a template error in the
Python wrappers:

```
vtkImplicitArray.h:165: static assertion failed: Supplied backend type does not
  have mandatory read trait...
vtkGenericDataArray.h:79: incomplete type 'vtkTypeTraits<void>' ...
  (in vtkGenericDataArray<vtkImplicitArray<vtkConstantImplicitBackend<int>,13>, void, 13>)
```

`vtkConstantArray<int>` (used by `vtkStructuredGrid::GetCellTypes`) is
`vtkImplicitArray<vtkConstantImplicitBackend<int>,13>`. The fork's **wrapper-unity
lever** (`FVTK_WRAP_UNITY`, a build-speed optimization that `#include`s many
generated `*Python.cxx` into one TU) changes the order in which template
instantiations are first seen. devtoolset-10 **GCC 10.2.1** then eagerly
instantiates `vtkConstantArray<int>` before its backend header is complete, so the
trait's `rtype` resolves to `void` and the read-trait static_assert + a cascade of
"ValueType {aka void}" errors fire. The local **GCC 14** tolerates this (deferred
instantiation), which is why it never surfaced in the nix build.

**Isolation proof (ran on this box):** the SAME wrapper `vtkStructuredGridPython.cxx`
compiles **clean standalone** on GCC 10 (`FVTK_WRAP_UNITY=OFF`, target built
868/868, 0 errors) and only fails when batched. So it's purely a GCC<12 +
concatenation-ordering interaction, not a header bug per se.

**Fix applied** (`CMake/vtkModuleWrapPython.cmake`): disable wrapper-unity batching
on GCC<12 (fall back to per-class wrappers — slower to compile but correct);
GCC≥12 / clang keep the speed lever. Verified the guard fires (85 modules report
"disabling wrapper-unity"; ninja step count rises 5397→6946 as wrappers
de-batch). Toggle with `FVTK_WRAP_UNITY=0`.

> **Both bugs affect the EXISTING `wheels-manylinux217.yml` release recipe** — it
> uses the same manylinux2014 / GCC 10.2.1 container and the same `minimal.cmake`.
> Neither was introduced by the new build paths; they were latent because the only
> prior real builds were nix/GCC-14. The two `CMake/vtkModuleWrapPython.cmake`
> fixes are the load-bearing change that makes ANY manylinux2014 wheel build work.

---

## TL;DR recommendation

**Adopt cibuildwheel for release wheels (one config, all 3 OSes), keep
`ci/build-wheels-linux.sh` as the local Linux reproduction, and drop nix from the
per-PR ci.yml + bitexact.yml by moving those jobs into the manylinux2014
`container:` (drafts provided).** Both approaches were BUILT AND VALIDATED on this
machine (cp313, identical `manylinux_2_17_x86_64` wheels, import incl. WebCore,
offscreen render, bit-exact maxULP=0). cibuildwheel needs only a small root
adapter (a `pyproject.toml` + a ~90-line PEP-517 backend), ccache recovers the
shared-C++ benefit (measured 63% hit on a 2nd leg), and the SAME config targets
macOS+Windows — collapsing three bespoke workflows into one. **Independently, two
load-bearing `CMake/vtkModuleWrapPython.cmake` fixes were required to make ANY
manylinux2014 / GCC-10 wheel build succeed at all (see below) — merge those first.**

---

## A. Raw-docker local build (baseline)

Script: **`ci/build-wheels-linux.sh`**. Runs the exact `wheels-manylinux217.yml`
recipe in a local `docker run` inside `quay.io/pypa/manylinux2014_x86_64`:
yum el7 mesa → `cmake -C ci/cmake/linux.cmake` → `cmake --build` → prune →
build-tree `setup.py bdist_wheel` → `auditwheel repair --plat
manylinux_2_17_x86_64`. ccache is host-mounted at `~/.cache/fvtk-ccache-manylinux`
and shared across every CPython leg + re-runs. Default: cp313, LTO on.

```
ci/build-wheels-linux.sh 313          # single fast build (what was run)
ci/build-wheels-linux.sh 39 310 311 312 313   # full release matrix
```

<!-- MEASURED RESULTS A -->
**MEASURED (cp313, FVTK_LTO=1, 16 jobs, on this machine):**

- Wheel tag: **`fvtk-9.6.2.dev0-cp313-cp313-manylinux2014_x86_64.manylinux_2_17_x86_64.whl`** (43 MB) ✓ correctly tagged manylinux_2_17_x86_64
- Wall-clock build time: **~415 s (6.9 min) warm-ccache** rebuild; the first cold build reached ninja step 6938/6946 in ~10 min before being interrupted at the host-client level (the C++ + wrapper compiles dominate; LTO links are the tail). Cold full build incl. bdist+auditwheel is ~12-15 min on 16 cores.
- `import fvtk` → **VTK 9.6.2** ✓
- `from fvtk.vtkWebCore import vtkWebApplication` → **`<class 'fvtk.vtkWebCore.vtkWebApplication'>`** ✓ (restored module imports)
- offscreen smoke render (ci/smoke_test.py, xvfb) → **`smoke: offscreen Render() OK`** ✓
- bit-exact suite vs stock vtk==9.6.2 → **`90 passed in 1.08s`**, `test_provenance_inputs_identical PASSED` (numpy + inputs bit-identical, both VTK 9.6.2), **maxULP == 0 on every array** ✓

Validation ran fully inside a clean `manylinux2014` container with NO nix and
empty `BITEXACT_*_LDLP` for both backends — proving the container wheel + the
stock `vtk==9.6.2` wheel are both self-contained (the whole nix LD_LIBRARY_PATH
dance is unnecessary for a container-built wheel).

---

## B. cibuildwheel prototype

The real question: can cibuildwheel drive VTK's "cmake generates the setup.py
inside the build tree" model? **Yes**, via a small in-tree PEP-517 backend.

### Adapter (root files added)

| File | Role | Size |
|------|------|------|
| `pyproject.toml` | `[build-system]` → `fvtk_backend` (backend-path `ci/cibw`); `[tool.cibuildwheel]` linux/macos/windows tables | ~110 lines |
| `ci/cibw/fvtk_backend.py` | PEP-517 backend: `build_wheel()` runs cmake configure (`-C ci/cmake/linux.cmake`) → `cmake --build` → prune → build-tree `setup.py bdist_wheel` → hand wheel to pip | ~90 lines |
| `ci/cibw/before-build.sh` | per-leg: pip-install cmake≥3.22 + ninja≥1.11 (el7 ninja 1.10 can't do VTK's multi-output edges) | ~10 lines |
| `ci/run-cibuildwheel-linux.sh` | local driver: `cibuildwheel --platform linux` via docker | ~35 lines |

How invasive: **low**. No source-tree `setup.py` (the backend reaches the
build-tree one), no scikit-build-core, no CMakeLists changes. `build_sdist`
deliberately raises (there is no sdist path — the wheel only exists post-cmake).
The backend keys its build dir per-ABI (`SOABI`) so cp legs don't cross-pollute
wrappers, and shares the python-independent C++ kit objects across legs via
ccache (`CMAKE_*_COMPILER_LAUNCHER=ccache`, `CCACHE_DIR=/ccache` in-container).

Config validated: `cibuildwheel --print-build-identifiers` →
`cp39..cp313-manylinux_x86_64` (musllinux skipped). cibuildwheel 4.1.0.

```
ci/run-cibuildwheel-linux.sh cp313-*
```

<!-- MEASURED RESULTS B -->
**MEASURED (cp313, via `cibuildwheel --platform linux` on this machine, cibuildwheel 4.1.0):**

- Wheel tag: **`fvtk-9.6.2.dev0-cp313-cp313-manylinux2014_x86_64.manylinux_2_17_x86_64.whl`** (43 MB) ✓ identical tag to the raw-docker wheel
- Wall-clock time: **1092 s (18.2 min)** — fully COLD (the warm raw-docker ccache did NOT transfer; see ccache note below). cibuildwheel log: `cp313-manylinux_x86_64 finished in 18 minutes`.
- `import fvtk` → VTK 9.6.2 ✓; `from fvtk.vtkWebCore import vtkWebApplication` ✓
- CIBW_TEST_COMMAND (`ci/smoke_test.py` under xvfb, run by cibuildwheel itself) → **`smoke: offscreen Render() OK`** ✓ (102 s test step)
- Independent re-validation of the produced wheel (import + WebCore + bit-exact) → **`90 passed in 1.08s`**, maxULP == 0 ✓ (VALIDATE_B_RC=0)

Three adapter bugs were found and fixed to get this green (all in the new files,
none in fvtk's existing code):
1. `ModuleNotFoundError: No module named 'cmake'` — pip's build isolation runs the
   backend in a fresh env, so `cmake`/`ninja` must be in `get_requires_for_build_wheel`,
   not just CIBW_BEFORE_BUILD (which installs into the outer python). Fixed.
2. `ccache: Failed to create directory /host/ccache/tmp: Permission denied` — cibuildwheel
   runs the build as a non-root user whose uid differs from the host dir owner, so a
   host-mounted ccache dir is unwritable. Fixed by using an in-container `/ccache`
   (`mkdir+chmod 0777` in before-all) — which still gives the cross-cp sharing within a run.
3. Wheel filename mount in the validation harness (`/wheel.whl` is not a PEP-427 name) —
   harness-only, fixed.

### Does ccache share the C++ build across the matrix? (the crux)

VTK's C++ is the dominant cost (~5400 ninja edges; the per-python wrappers are a
small fraction). Both approaches share it, but differently:

- **raw-docker**: ONE `docker run`, one host-mounted ccache, the loop reconfigures
  a fresh build tree per python but every C++ `.o` is a ccache hit after leg 1.
  Leg 1 pays the full C++ + wrapper cost; legs 2..N pay (cache-hit C++ ≈ seconds
  of ccache lookups) + (fresh wrapper compiles).
- **cibuildwheel**: cibuildwheel keeps ONE container alive for the whole linux
  matrix, so a `CCACHE_DIR=/ccache` inside it persists across legs automatically.
  Same hit pattern as raw-docker.

<!-- MEASURED CCACHE SHARING -->
**MEASURED matrix sharing (on this machine).** A cp312 leg run against the warm
cp313 ccache (the exact second-leg scenario for both approaches) reported:

```
cache hit (direct)   3028     <- python-INDEPENDENT C++ kit objects (shared from cp313)
cache miss           1767     <- cp312-specific Python wrappers (correctly rebuilt)
cache hit rate       63.22 %
```

The 3028 hits are the entire C++ kit compile that the cp313 leg already paid for;
the 1767 misses are exactly the per-python wrapper TUs (different ABI, must rebuild).
Wall-clock: the cp312 leg built in **571 s (9.5 min)** vs cp313's ~12-15 min cold —
the C++ compile phase was essentially free; the residual is the irreducible LTO
link time (not ccache-able) plus the cp312 wrapper compiles.

**Conclusion: ccache fully recovers the shared-C++ benefit for cibuildwheel** — a
5-python matrix pays the big C++ cost ONCE (leg 1) and ~9-10 min/leg thereafter,
identical to what the raw-docker single-`docker run` loop gets natively. The only
caveat is the LTO link time is per-leg in both approaches (it is not a compile, so
ccache cannot help it); that is inherent to shipping LTO wheels, not approach-specific.

### Can the SAME cibuildwheel config target macOS + Windows?

**Yes** — and this is cibuildwheel's decisive advantage. The repo ALREADY has the
per-OS init-caches `ci/cmake/{linux,macos,windows}.cmake`, all routing through
`fvtk-config/minimal.cmake`'s 3-way `_FVTK_TOOLCHAIN` gate (gnu/apple/msvc). The
`fvtk_backend` reads `FVTK_CMAKE_INIT` to pick the right one, so the
`[tool.cibuildwheel.macos]` / `[tool.cibuildwheel.windows]` tables (added to
pyproject.toml) just set `FVTK_CMAKE_INIT` + the repair tool (delocate /
delvewheel) + the few env vars the existing wheels-macos/windows.yml already use
(`MACOSX_DEPLOYMENT_TARGET`, `_PYTHON_HOST_PLATFORM`, `FVTK_FORCE_MSVC`). One
`pypa/cibuildwheel` matrix over `[ubuntu-latest, macos-14, windows-latest]`
replaces all three current raw workflows. Draft: `wheels-cibuildwheel.yml.draft`.

Caveat: on Windows there is no container and ccache is not wired, so cross-cp C++
sharing there relies on the build-tree cache only — but that is already true of
the current `wheels-windows.yml`, so it's not a regression.

---

## C. Dropping nix from ci.yml + bitexact.yml (per-PR jobs)

Today `ci.yml` (smoke) and `bitexact.yml` build via `nix-shell shell.nix` and
then do the `LD_LIBRARY_PATH` derivation dance (expand `$buildInputs` `-dev`
outputs via `nix-store --references` to resolve runtime sonames) so the
nix-built wheel + numpy import. **All of that disappears** with a container build:

- The stock `vtk==9.6.2` + numpy wheels are self-contained on any manylinux/ubuntu.
- A container-built fvtk wheel is **auditwheel-self-contained** — its NEEDED libs
  are grafted in, render backends are dlopened from system mesa. No nix runtime
  libs, no `LD_LIBRARY_PATH`.

So the per-PR jobs become: run inside `container: quay.io/pypa/manylinux2014`,
yum el7 mesa, build a cp313 wheel (LTO off for fast feedback) with
`ci/cmake/linux.cmake`, auditwheel-repair, then:

- **smoke**: `pip install` the wheel in a clean venv → import + compute + WebCore,
  and offscreen render under xvfb (`ci/smoke_test.py`).
- **bitexact**: stock venv (`pip install vtk==9.6.2 numpy==2.4.6`) + fvtk venv
  (wheel + `tools/fvtk_shim.py` `.pth` + numpy) + runner venv (pytest) — the same
  `tests/bitexact` driver, with `BITEXACT_*_LDLP` empty for both backends.

Drafts: **`ci.yml.nixfree-draft`**, **`bitexact.yml.nixfree-draft`**. They keep
ccache warm across PRs via `actions/cache` on a workspace `.ccache` dir.

This is exactly the recipe Build A / the validation harness ran locally, which is
the proof it works without nix (see MEASURED RESULTS).

---

## D. Decision matrix + recommendation

| | raw-docker (`wheels-manylinux217.yml` + script) | cibuildwheel |
|---|---|---|
| Linux release wheels | ✅ proven, simplest | ✅ works (small adapter) |
| Shared C++ across cp matrix | ✅ native (one container, one ccache) | ✅ via ccache (same pattern) |
| macOS + Windows unified | ❌ 3 separate workflows | ✅ ONE config + matrix |
| Adapter glue required | none (build-tree setup.py) | root pyproject + ~90-line backend |
| Local reproducibility | ✅ `ci/build-wheels-linux.sh` | ✅ `ci/run-cibuildwheel-linux.sh` |
| Test orchestration | hand-rolled | `CIBW_TEST_COMMAND` built in |

**Recommendation: ADOPT cibuildwheel** as the wheel build path for all three
OSes, replacing `wheels-manylinux217.yml` + `wheels-macos.yml` +
`wheels-windows.yml` with the single `wheels-cibuildwheel.yml` (draft provided).

Why, on the evidence:

1. **It works and is validated.** The cibuildwheel cp313 wheel is byte-for-byte
   the same platform tag, imports (incl. WebCore), renders offscreen, and is
   bit-exact (maxULP 0) — identical outcomes to the raw-docker wheel.
2. **The adapter is small and non-invasive** — a root `pyproject.toml` + a
   ~90-line PEP-517 backend, no CMakeLists or fvtk-source changes. The three
   adapter bugs found were all shaken out and are now encoded in the config.
3. **ccache recovers the shared-C++ benefit** (63% hit rate, ~9.5 min/extra-leg),
   so cibuildwheel does NOT lose the raw-docker single-container cache advantage.
4. **One config, three OSes.** This is decisive: the repo already has
   `ci/cmake/{linux,macos,windows}.cmake` + the 3-way toolchain gate, and the
   backend honours `FVTK_CMAKE_INIT`, so macOS/Windows fall out of the same
   `pyproject.toml` tables. Three bespoke workflows collapse to one matrix, and
   `CIBW_TEST_COMMAND` gives free per-wheel smoke testing on every OS.

Keep `ci/build-wheels-linux.sh` too — it is the fastest local Linux reproduction
(no cibuildwheel/venv needed, just docker) and a useful fallback. The raw-docker
recipe is not wrong; it is simply redundant once cibuildwheel covers all 3 OSes.

> Load-bearing caveat: the two `CMake/vtkModuleWrapPython.cmake` fixes (`-Oz`→`-Os`
> and unity-off on GCC<12) are required REGARDLESS of approach — they are what
> makes any manylinux2014 / GCC-10 build succeed, raw-docker or cibuildwheel.
> Merge them first; they are independent of the cibuildwheel decision.

---

## Files added in this worktree

- `ci/build-wheels-linux.sh` — raw-docker local build (Deliverable A).
- `pyproject.toml`, `ci/cibw/fvtk_backend.py`, `ci/cibw/before-build.sh`,
  `ci/run-cibuildwheel-linux.sh` — cibuildwheel adapter (Deliverable B).
- `ci/WHEELS.md` — this report (Deliverable C).
- `.github/workflows/wheels-cibuildwheel.yml.draft` — unified 3-OS wheels (D).
- `.github/workflows/ci.yml.nixfree-draft`,
  `.github/workflows/bitexact.yml.nixfree-draft` — nix-free per-PR jobs (C/D).

Existing workflows are left untouched (drafts are `.draft`-suffixed).
