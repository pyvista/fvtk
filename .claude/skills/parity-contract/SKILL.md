---
name: parity-contract
description: The fvtk bit-exact parity contract — the two buckets (byte-identical default-on vs opt-in EnableFast), the bitexact / renderexact / regression gates, how to run them locally, and the checklist for adding a fast filter. Load when touching any filter, kernel, render path, or build lever, or when reasoning about whether a change can ship on by default.
---

# The fvtk parity contract

fvtk is a **bit-exact drop-in for stock VTK 9.6.2 by default** (`maxULP = 0`). This is the one
rule everything else serves. The authoritative text is `CONTRIBUTING.md`; the lever set and the
gate internals are in `docs/build-internals.md`. This skill is the working summary.

## The two buckets

Every change is sorted by whether its output is byte-identical to stock VTK 9.6.2.

**Bucket 1 — byte-identical → ships on by default.**
Must pass `tests/bitexact/` at `maxULP = 0`, and for anything threaded, the thread-count
determinism check (1, 4, 8 threads). This covers devirtualization, LTO/PGO, SIMD (FMV, built
with `-ffp-contract=off` so no FMA divergence), int-width relaxation that cannot overflow for
the bound, and thread-count-invariant parallel loops. Examples already shipped: `vtkWarpVector`,
`vtkWarpScalar`, `vtkPolyDataNormals`, `vtkElevationFilter`, `vtkThreshold`.

**Bucket 2 — correct but reorders output → opt-in behind `fvtk.EnableFast()`.**
Gated on `fvtk::FastModeEnabled()` (env `FVTK_FAST`, process-global, runtime-switchable). Cell
emission order is negotiable; the point set and the multiset of cells are not. Validated with a
relaxed-order parity mode plus an engagement check that proves the fast kernel actually ran (a
fast path that silently falls back to stock and still "passes" is a bug). Examples:
`EnableFast` parallel `vtkClipPolyData`, parallel union-find connectivity labeling.

**Sacred at all times:** positions and values. A kernel that changes the math, even by one ULP,
is a different algorithm, not a reordering, and does not qualify for either bucket as a drop-in.

## Backports are the deliberate exception

A backported upstream bug fix intentionally diverges from stock 9.6.2 because stock is wrong.
It will fail a naive bitexact comparison for the buggy case. That is expected. Record the
divergence (the bitexact op for the affected path is updated to the corrected expectation, and
the reason is noted in the divergence ledger / `tests/precision_audit.md`), and link the
upstream MR. For pure crash/guard fixes on degenerate input, valid-input output is unchanged, so
byte-exactness still holds for the normal cases. See the `backport-vtk` skill.

## The gates

| Gate | Script | What it proves |
|---|---|---|
| bitexact | `ci/run-bitexact.sh` | Array output byte-identical to stock 9.6.2, `maxULP = 0` |
| renderexact | `ci/run-renderexact.sh` | Offscreen RGBA+Z framebuffer byte-identical (EGL/Mesa) |
| regression | `ci/run-regression.sh` | fvtk's own regression scenes pass |
| pyvista | `ci/run-pyvista.sh` | PyVista's own suite passes against the built wheel (PR #112) |

All four install stock `vtk==9.6.2` and the fvtk wheel on separate venvs and redirect
`vtkmodules` → `fvtk` via `tools/fvtk_shim.py`. **Never install stock `vtk` into the fvtk
venv** — an un-redirected import must fail loud rather than silently test stock.

## Running bitexact locally

```bash
export BITEXACT_STOCK_PY=<stock-venv>/bin/python     # has vtk==9.6.2
export BITEXACT_FVTK_PY=<fvtk-venv>/bin/python        # has the fvtk wheel
export BITEXACT_FVTK_LDLP=/nix/store/<gcc>/lib:...    # nix libs the fvtk wheel needs
cd tests/bitexact
python -m pytest -v
python -m pytest -m modified    # hard gate: only the 9 modified filters
```

`tests/bitexact/` layout: `ops.py` (the cases), `run_ops.py` (runs each case on both
interpreters, dumps `.npz` + `manifest.json`), `compare.py` (asserts byte-equality; integer
arrays width-normalized first), `test_bitexact.py` (the parametrized pytest), and
`test_smp_determinism.py` (thread-count parity). Full notes in `tests/bitexact/README.md`.

## Adding a Bucket-1 (default-on) optimization

1. Implement the faster path so its output is byte-identical to stock for every dtype and size.
2. If threaded, make it thread-count invariant (deterministic reduction order, no
   nondeterministic atomics on float).
3. Add or extend a `tests/bitexact/` op covering the path, with coordinate-derived attribute
   data so the values exercise the kernel.
4. Run `ci/run-bitexact.sh` (and `test_smp_determinism.py` for threaded paths). `maxULP` must
   be 0.
5. State the bucket and attach the bitexact result in the PR.

## Adding a Bucket-2 (opt-in `EnableFast`) filter

1. Gate the fast path on `fvtk::FastModeEnabled()`. Keep the stock path reachable and exact when
   fast mode is off.
2. Vendored parallel kernels go in a separate non-unity translation unit, compiled under OpenMP
   behind `FVTK_HAVE_OPENMP`, with automatic fallback to the stock path.
3. Add a `tests/bitexact/` op with the relaxed flag (`order_relaxed` or `points_relaxed`) and
   coordinate-derived attributes.
4. Add an engagement check that proves the fast kernel ran under `EnableFast()`.
5. Verify the default build (fast off) still passes at `maxULP = 0`.

## Common traps

- NOCOMPILE only filters classes; classes pulled in via SOURCES/TEMPLATES still compile. Do not
  delete their files. (See the `module-trimming` skill.)
- C++ source-unity batching is hostile in VTK (anonymous-namespace collisions); only
  wrapper-unity is safe.
- SIMD kernels must compile with `-ffp-contract=off` or FMA contraction breaks byte-exactness.
- Prove deletions and configure changes in a fresh build dir; a dirty cache masks generate-time
  breakage.
