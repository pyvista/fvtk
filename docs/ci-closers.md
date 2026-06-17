# CI closers: getting the fvtk PR gate to ≤20 min (ideal 10) on standard runners

Goal: drive the per-PR CI gate (`.github/workflows/ci.yml`) under 20 minutes
(ideally 10) on **standard** GitHub runners (ubuntu-latest = 4 vCPU), with every
lever **bit-exact** (the wheel stays maxULP=0 vs stock VTK 9.6.2). Larger runners
are off the table.

This work is BEYOND the compile reductions already consolidated on
`perf/manylinux228` (manylinux_2_28 / GCC-14, source + wrapper unity, dispatch
6→4, array-TU split, single cp312-abi3 wheel). Those levers cut **compile**; this
doc quantifies how much of the gate is compile vs not, and adds levers that the
unity/2_28 work cannot reach.

## What the PR gate actually is

`ci.yml` has three job groups on plain `ubuntu-latest`:

- **build** (matrix, 2 parallel legs on 2 separate runners):
  - `cp311 static` and `cp312-abi3`. Each leg starts its OWN fresh
    manylinux_2_28 container and does its OWN cold cibuildwheel build — the two
    legs do **not** share ccache (separate runners). On the PR gate each leg
    builds exactly ONE python (no cross-cp sharing within a leg either), so the
    "ccache shared across the cp matrix" note in pyproject does not help the gate.
  - `CIBW_ENVIRONMENT: FVTK_LTO=0 FVTK_SOURCE_UNITY=1`.
- **bitexact** (`needs: build`): per-python, installs the wheel + stock vtk, runs
  the byte-exact suite. Tiny compute; cost is venv + pip + suite ≈ 1–3 min.
- **renderexact** (`needs: build`): apt-installs Mesa, runs the pixel-exact render
  suite. Cost ≈ 1–3 min, runs in parallel with bitexact.

So the **gate wall ≈ slowest build leg + max(bitexact, renderexact)**. The test
jobs are minor; the build leg is the whole game. The measurement target is
therefore exactly: **one cold cibuildwheel build leg in manylinux_2_28 at -j4**
(ubuntu-latest = 4 vCPU = the CI parallelism).

Measured on the executor (32-core, rootless docker) in the real
`quay.io/pypa/manylinux_2_28_x86_64` container, cp312-abi3 leg, -j4.

## Measurement caveat (executor contention)

The executor is shared and raced by other build agents. Absolute wall numbers are
inflated by background load (other agents' `cc1plus` saturate cores); rootless
docker on this box also silently DISCARDS `--cpuset-cpus` ("kernel does not
support cpuset"), so builds cannot be pinned. Therefore:
- Fixed, low-CPU phases (before-all, deps, bdist, auditwheel, smoke) are reliable.
- `configure` and `build` are load-sensitive; the A/B **ratios** (o3-vs-o2,
  cold-vs-warm), measured in the SAME contention window, are the trustworthy
  signal — not the absolute seconds, which a dedicated 4-vCPU ubuntu-latest runner
  would reproduce more cleanly (and likely faster, with no co-tenant builds).

## Phase breakdown — build leg (cp312-abi3, -j4, FVTK_LTO=0 FVTK_SOURCE_UNITY=1)

Fixed phases (validated, load-insensitive):

| phase | what | wall (s) | notes |
|-------|------|---------:|-------|
| before-all | `dnf install` render stack + ccache + ninja + git | ~27–30 | once per container |
| build-deps | `pip install cmake/ninja/setuptools/wheel` | ~3.5 | |
| configure | `cmake -S -B -C linux.cmake` (NO compile) | ~31 (uncontended) – 53–66 (contended) | **non-compile sink**: VTK module graph + wrapper-hierarchy setup |
| build | `cmake --build` (ninja): codegen + compile + link | cold ~730–760 (contended); warm **99.7** | dominant; see codegen/compile/link split below |
| bdist | prune_setup_py + `setup.py bdist_wheel` | ~3.6 | stages the wheel |
| auditwheel | `auditwheel repair --plat manylinux_2_28` | ~22 | ELF scan/patch/vendor — real non-compile cost |
| smoke | import (+ offscreen render in CI) | ~6 | cibuildwheel test-command |

**Fixed non-compile overhead per leg ≈ before-all 28 + deps 3.5 + configure 31 +
bdist 3.6 + auditwheel 22 + smoke 6 ≈ 94 s (~1.6 min)**, independent of the
compile work. This is the floor the build phase sits on top of.

### Inside the build phase: codegen vs compile vs link

From the **100 %-ccache-hit warm build** (0 compile misses → exposes exactly the
non-cacheable floor), the per-step durations are unambiguous:

- **Links dominate the floor.** The longest steps are all `.so` links —
  `libvtkCommon.so` 43 s, `libvtkFilters.so` 19 s, `vtkCommonCore.abi3.so` 8 s,
  `libvtkOpenGL.so`/`libvtkParallel.so`/`libvtkIO.so` ~7 s each, then a long tail
  of 146 kit + wrapper `.so`. Links are NEVER ccache-able. On 4 cores the link
  set is the warm critical path.
- **Codegen is cheap and parallel.** Every individual `vtkWrapPython` / hierarchy
  step is sub-second (longest: `vtkWrapPython` 0.9 s, a `*-hierarchy.txt` 0.66 s).
  They are ordinary ninja `add_custom_command` edges, scheduled across all `-j`
  workers — never a serial long-pole.
- **Compiles, when cached, are ~instant** (ccache hit ≈ fork + copy).

**Key structural findings:**
1. **Wrapper generation is already parallel and cheap** — no serial wrapper-gen
   long-pole exists to fix (lever 3 is a non-lever; see §3).
2. **The warm-build floor is the LINK set** (+ quick codegen + cache-fast
   compiles). Links are not ccache-able, so a perfectly-seeded build still pays
   them: measured **99.7 s** at 100 % hit. That is why a warm build is not instant
   — and it is still far under the targets.

## Levers (each measured at -j4)

### 1. -O2 for the PR gate (release stays -O3) — `FVTK_GATE_O2=1`

Implemented in `fvtk-config/minimal.cmake` (overrides `CMAKE_CXX_FLAGS_RELEASE`/
`CMAKE_C_FLAGS_RELEASE` to `-O2 -DNDEBUG` when `FVTK_GATE_O2=1`; release leaves it
unset → CMake's GNU Release default `-O3 -DNDEBUG`). Wired into the PR-gate
`CIBW_ENVIRONMENT` next to `FVTK_LTO=0`.

**Bit-exact: PROVEN.** The actual -O2 wheel (cp312-abi3, FVTK_LTO=0 FVTK_GATE_O2=1)
was run through the repo's `tests/bitexact` suite vs stock VTK 9.6.2:
**160 passed, maxULP=0** — byte-for-byte identical. (Mechanism: without
`-ffast-math`, GCC honours strict IEEE FP at both -O2 and -O3 and does not
reassociate FP or vectorize FP reductions, so numeric output is identical. Same
safety logic as the existing LTO-off gate. The override is confirmed active:
`CMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG` in the configured CMakeCache.) Side bonus:
the -O2 wheel is **38.9 MiB vs 42.8 MiB** at -O3 (less inlining/unrolling) — gate
artifact only; release stays -O3.

**Measured build-time delta at -j4 (cp312-abi3, cold, executor under contention):**

| | configure | build | total leg |
|--|----------:|------:|----------:|
| -O3 (o3p) | 30.7 | **759.3** | ~856 s |
| -O2 (o2p) | 30.4 | **732.6** | ~842 s |

A focused single-TU compile A/B (isolated, best-of-3) on a CommonCore
instantiation TU: -O3 2.01 s → -O2 1.92 s (**-4.5 %**). Mid-build ninja-step
progress had the -O2 leg running ~5–6 % ahead of the -O3 leg through the bulk
module compiles.

**Honest read: -O2 is a real, free, bit-exact win but a MODEST one — ~4–6 % of
build time.** Two reasons it isn't bigger: (a) the heavy CommonCore
array-instantiation TUs (the -j4 long pole) are nearly the same cost at -O2 vs
-O3; (b) a large fraction of the build is the non-cacheable **link + codegen
floor**, which -O2 doesn't touch. The A/B also ran the two legs in parallel
sharing cores, so when the faster -O2 leg finished it freed cores to the -O3
leg's tail — which *understates* the -O2 advantage; the true clean-runner gap is
toward the upper end of 4–6 %. Worth taking (it costs nothing), but it does not by
itself move the cold gate into the 10-minute zone.

### 2. ccache seed (cold≈warm for PR iteration) — the lever that hits ≤10

The PR gate today has **zero** ccache persistence: each of the 2 build legs runs
on its own fresh ubuntu-latest runner and the in-container `/ccache` is discarded
at container exit, so **every PR build is fully cold**. (The pyproject note about
"ccache shared across the cp matrix" only helps *within* a single multi-python
cibuildwheel run — the gate builds one python per leg, so it never benefits.)

The fix (implemented in `.github/workflows/ci.yml` + `CIBW_CONTAINER_ENGINE`):
1. `push: branches: [main]` build refreshes a per-leg ccache and SAVES it under
   `fvtk-ccache-<leg>-<run_id>`.
2. PR-gate build restores the latest seed via `restore-keys: fvtk-ccache-<leg>-`
   (branch scoping lets a PR read its base branch main's cache).
3. `CIBW_CONTAINER_ENGINE: "docker; create_args: -v /tmp/fvtk-cibw-ccache:/ccache"`
   mounts the restored host dir into the build container so ccache actually
   reads/writes the seed (CCACHE_DIR=/ccache is already set in pyproject).

The C++ kit objects are python-independent and byte-stable across commits that
don't touch them, so a PR whose diff is a handful of files restores the seed and
recompiles only its changed TUs (+ the non-cacheable codegen/link floor).

**Measured warm build at -j4 (seed = a complete cold -O3 build's ccache):**

| | hit rate | build | leg total (with overhead) |
|--|---------:|------:|--------------------------:|
| cold (no seed) | 0 % | ~730–760 s* | ~14 min* |
| warm (full seed) | **100 %** (3893 hit / 0 miss) | **99.7 s** | **~3.5 min** |

\* under ~2× executor contention; a clean dedicated 4-vCPU runner is faster.

The warm build-total of **99.7 s with 0 recompiles** IS the pure non-cacheable
floor (the 146 links dominate, + quick codegen + ninja overhead on 4 cores). A
real PR that edits a handful of files adds only those TU recompiles (~1–5 s each)
on top — the floor dominates. **A warm PR build leg lands at ~3.5–4 min**, which
puts the COMMON case (PR iteration) comfortably ≤10 min. This is the single lever
that reaches the 10-minute goal; -O2 alone cannot.

First-ever PR on a brand-new cache key (no seed yet) still pays the full cold
build (~12–14 min) — under 20, not 10. The `push: branches: [main]` seed-refresh
keeps the common case warm.

### 3. Wrapper-generation parallelism — NOT a lever (already parallel)
The `vtkWrapHierarchy`/`vtkWrapPython` codegen steps are ordinary ninja edges and
already run across all `-j` workers, interleaved with compiles (see structural
finding #1). Each step is sub-second (longest: `vtkWrapPython` 0.9 s, a per-module
`*-hierarchy.txt` 0.66 s); there is no serial wrapper-gen long-pole to fix. The
per-module hierarchy file (every module's wrappers depend on it) is the only true
serialization point, and it is cheap and off the critical path. **No change
recommended.** The only realizable codegen win would be NOT generating wrappers
for classes pyvista never calls — already done (`_nowrap_classes.cmake`, ~1175
classes skipped).

### 4. auditwheel repair — ~22 s, minor
auditwheel scans the wheel's `.so`s, vendors external deps, patches RPATHs, stamps
the 2_28 tag. ~22 s on the ~41 MiB LTO-off gate wheel. It is on the critical tail
(build → bdist → audit), but it is ~2 % of a 15-min gate. Not worth optimizing;
no bit-exact risk either way. (If ever needed: auditwheel's ELF patching is the
cost; there is no safe way to skip it while keeping a valid manylinux tag.)

### 5. Other bit-exact levers — evaluated
- **Minimal-module gate build: REJECTED (unsound).** The bitexact + renderexact
  gates exist to prove the SHIPPED wheel == stock VTK 9.6.2. Building a smaller
  module closure for the gate would validate an artifact you do not ship, breaking
  the contract. The render gate also needs the full Rendering/OpenGL2/EGL stack.
  No safe footprint reduction here.
- **PCH where unity doesn't reach: REJECTED (redundant).** `VTK_USE_PCH=OFF` and
  source+wrapper unity already amortize the shared-header parse (the thing PCH
  would buy), strictly better. Adding PCH on top is redundant churn.
- **Drop dead wrapper tools (`vtkWrapJavaScript`, `vtkWrapSerDes`,
  `vtkParseJava`): small, real, bit-exact.** The build still links
  `bin/vtkWrapJavaScript`, `bin/vtkWrapSerDes` etc. every run though the Python-
  only wheel never uses them. Gating them off (a separate in-flight branch already
  prototyped this) shaves a few tool-link steps. Marginal (~seconds), folds in
  cleanly. Not measured here as a headline lever; noted for the consolidation.

## Recommended stack

Combine, on top of the in-flight 2_28 + unity + array-split + single-abi3-wheel
consolidation:

1. **ccache seed** (lever 2) — the decisive lever. Restore a per-leg ccache seeded
   by the `push: branches: [main]` build; mount it into the cibuildwheel container.
   Turns the COMMON case (PR iteration on a warm cache) into a **~3.5-min** build
   leg (100 % hit → 99.7 s build + overhead). This is what reaches ≤10.
2. **-O2 gate** (lever 1, `FVTK_GATE_O2=1`) — free, bit-exact (160/160 maxULP=0),
   ~4–6 % off the COLD build. Trims the cold worst-case (no-seed) margin and the
   recompile cost of large PRs. Release stays -O3.

Both are in `perf/ci-closers`:
- `fvtk-config/minimal.cmake`: `FVTK_GATE_O2` block.
- `.github/workflows/ci.yml`: `push: [main]` trigger; per-leg `actions/cache`
  restore/save; `CIBW_ENVIRONMENT += FVTK_GATE_O2=1`;
  `CIBW_CONTAINER_ENGINE` ccache mount.

### Will it hit the targets?

| scenario | build leg | gate wall (build + ~2–3 min tests) | ≤20? | ≤10? |
|----------|----------:|-----------------------------------:|:----:|:----:|
| **warm** (PR iteration, seed present) — the common case | ~100 s | **~5–7 min** | ✅ | ✅ |
| **cold** (first build on a fresh cache key) | ~600–700 s clean (−O2) | **~12–14 min** | ✅ | ❌ |

**Honest verdict:**
- **≤20 min: already met** by the in-flight 2_28 + unity work; -O2 adds margin.
- **≤10 min cold (no cache): NOT reachable** on a standard 4-vCPU runner. The
  VTK C++ closure is simply too large to compile from scratch in 10 min at -j4,
  and the bit-exact constraint forbids the real shortcut (dropping modules /
  fast-math / a smaller gate artifact). -O2 only buys ~5 %.
- **≤10 min warm: reachable and is the realistic common case.** With the ccache
  seed every PR after the first-on-a-key restores main's cache and rebuilds in
  ~3.5 min. Iterating on a PR (push after push) is always warm.

The path to a reliably-≤10 gate is therefore "keep the cache warm," not "compile
faster" — the seed lever delivers it; -O2 is a free bit-exact bonus that tightens
the cold tail.

### Levers considered and NOT taken (bit-exact accounting)
- Wrapper-gen parallelization (already parallel), PCH (redundant with unity),
  minimal-module gate build (unsound — wouldn't validate the shipped artifact),
  auditwheel speedups (~22 s, not worth it). See sections 3–5.
- Dead wrapper tools (`vtkWrapJavaScript`/`vtkWrapSerDes`) still build every run —
  a few seconds; a separate branch already prototypes gating them. Fold in if
  convenient; not a headline.
- **ICF-off on the gate (attack the link floor): TESTED, NOT recommended.** Since
  links are the warm floor and aren't ccache-able, dropping gold `--icf=all`
  (`FVTK_ICF=0`, gate-only, bit-exact — ICF is layout/size only) was an obvious
  candidate to speed linking. Measured the opposite/at-best-flat: a warm
  ICF-off build's big-kit links were *not* faster than ICF-on in the runs I could
  get (libvtkCommon/libvtkFilters link wall was higher, but under heavier
  co-tenant load — the comparison is contention-confounded). No clean win
  demonstrated, so it is NOT in the recommended stack. Worth a clean re-test on a
  quiet/dedicated runner if squeezing the warm link floor ever matters; do not
  ship it on the strength of these numbers.

## Reproduction

All measured on the executor in `quay.io/pypa/manylinux_2_28_x86_64`, cp312-abi3
leg, -j4, from a clean `git archive` of `perf/ci-closers`. Harness:
`phase-measure.sh` (per-phase timing), `classify2.py` (ninja_log codegen/compile/
link split), `tu-bench.sh` (isolated single-TU -O2/-O3 A/B), `bitexact-o2.sh`
(repo `tests/bitexact` vs stock vtk 9.6.2 against the -O2 wheel).
