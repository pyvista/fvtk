# cvista bit-exactness regression suite

Asserts that **cvista** (this VTK 9.6.2 hard-fork) produces **byte-for-byte
identical** compute output to **stock VTK 9.6.2** across a broad set of filters
and `vtkCommon` operations. Tolerance is exact: `np.array_equal` on raw bytes of
*every* output array, `maxULP == 0`.

## How it works

One driver, two backends. Every operation in `ops.py` is written against the
`vtkmodules` API only (no pyvista, no `import vtk`). `run_ops.py` runs the whole
registry and dumps one `.npz` per case plus a `manifest.json`. It is run twice:

| backend | how `vtkmodules` resolves |
|---------|---------------------------|
| stock   | `pip install vtk==9.6.2` -> upstream `vtkmodules` |
| cvista    | the `tools/cvista_shim.py` redirect: `vtkmodules.* -> cvista.*` |

The only thing that differs between the two runs is the compiled C++ backend, so
any byte difference is attributable to cvista's build.

`compare.py` then asserts byte-equality on every array; `test_bitexact.py`
parametrizes one pytest case per `(op, dtype, size)`.

### Determinism

numpy is pinned to the **same** version (`2.4.6`) on both venvs so inputs are
bit-identical. Inputs use only deterministic ops (`arange`, `linspace`, integer
index algebra, `sqrt`) — never `sin`/`cos`, whose last-ULP results can drift
across libm builds and masquerade as an cvista divergence. `run_ops.py` records an
**inputs digest**; `test_provenance_inputs_identical` fails if the two sides did
not start from identical bytes.

## Coverage (78 bit-exact cases, 278 arrays)

* **9 modified filters** — hard gate, `-m modified`: `decimate, smooth, normals,
  contour, clip, threshold, warp, glyph, cell2point`.
* **Broader filters**: `point2cell, elevation, warpvector, clean, triangle,
  geometry, shrink, connectivity, featureedges, stripper, vertexglyph,
  decimatepro, cone, tube, gradient, cutter`. The last three plus `clean,
  connectivity, geometry` are wave-2 optimization targets (tube's per-vertex
  point-data copy, gradient's per-component loop, cutter's 3D-cell merge path,
  which `GetCellEdgeNeighbors` in the vtkCommon group also guards).
* **vtkCommon ops**: `vtkDataArray`/AOS round-trip + ranges, `vtkPoints` +
  bounds, `vtkPolyData.BuildLinks` + `GetPointCells`/`GetCellPoints`/
  `GetCellEdgeNeighbors`, `vtkUnstructuredGrid` construction + BuildLinks,
  `vtkCellArray`, `vtkMath` (cross/dot/norm/determinant), and point-locator
  queries (`vtkPointLocator`, `vtkStaticPointLocator`, `vtkMergePoints`).

Parametrized over dtype (`float32`/`float64` where the op carries a scalar field)
and 2 mesh sizes each.

## Run locally

Set two pythons — one with stock VTK, one with the cvista wheel + shim — and a
runner python with `pytest`+`numpy`:

```bash
export BITEXACT_STOCK_PY=/path/to/stock-venv/bin/python
export BITEXACT_CVISTA_PY=/path/to/cvista-venv/bin/python
# cvista wheel built under nix needs the nix runtime libs on its loader path:
export BITEXACT_CVISTA_LDLP=/nix/store/<gcc-lib>/lib:/nix/store/<zlib>/lib
export BITEXACT_STOCK_LDLP=...   # only if stock vtk isn't self-contained
cd tests/bitexact && python -m pytest -v
# hard gate only:
python -m pytest -m modified
```

CI: `.github/workflows/bitexact.yml`.
