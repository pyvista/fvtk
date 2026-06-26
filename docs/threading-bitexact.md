# Threading and bit-exactness

cvista ships **`Sequential`** as the default `vtkSMPTools` backend, the same as
stock VTK. This is deliberate: with the Sequential backend, cvista's compute output
is byte-for-byte identical to stock VTK 9.6.2 (`maxULP == 0`; see
`tests/bitexact/`), so cvista is a drop-in replacement.

VTK also compiles in an **`STDThread`** backend (std::thread). VTK 9.6's threaded
filters are written "gather-over-output" (each thread writes a disjoint output
range and a fixed-order gather assembles the result), so *most* of them produce
the **same bytes** threaded as sequential — but not all. Switching the global
backend to `STDThread` is therefore **not** an unconditional drop-in.

## Opting into STDThread

```python
import pyvista as pv
pv.enable_smp_tools(backend="stdthread")   # process-wide
```

or directly:

```python
from cvista.vtkCommonCore import vtkSMPTools
vtkSMPTools.SetBackend("STDThread")        # call before constructing filters
```

Speedup only materializes at **production mesh scale**. On small meshes thread
spin-up dominates and `STDThread` is often *slower* than `Sequential` — do not
expect a win on toy inputs.

## Bit-exact-under-threading allowlist

Audited Sequential vs STDThread at thread counts {1, 2, 4, 8}, comparing every
output array (points, all point/cell data, full topology) against **both**
cvista-Sequential **and** stock VTK 9.6.2, with each threaded config run twice to
catch run-to-run nondeterminism.

**SAFE — byte-identical to stock VTK at every thread count, run-to-run stable
(`maxULP == 0`):**

```
decimate, smooth, normals, contour, clip, threshold, warp, glyph, cell2point,
point2cell, elevation, warpvector, clean, triangle, shrink, connectivity,
featureedges, stripper, vertexglyph, decimatepro, cone, tube, gradient, cutter
```

(24 filters, including all of the bit-exact-gated "modified" filters.)

## NOT safe under threading

* **`vtkGeometryFilter`** — under `STDThread` its polygon connectivity
  (`conn:polys`) is a **thread-dependent, run-to-run-nondeterministic**
  permutation of the cell order. The connectivity *multiset* is preserved
  (points, offsets, and scalars are stable), so the geometry is correct, but the
  byte stream is not reproducible and not equal to stock VTK. **Keep
  `vtkGeometryFilter` on the Sequential backend.** If you enable `STDThread`
  process-wide and need reproducible / drop-in geometry extraction, run that
  filter under a Sequential scope, or use `vtkDataSetSurfaceFilter` where it fits.

## How this was measured

`tests/bitexact/` drives the same `vtkmodules`-API ops under two backends and
asserts byte-equality. The threading audit reused that op registry, flipping the
SMP backend per subprocess. Re-run the allowlist audit after any change to a
threaded filter, and re-benchmark speedups on representative production meshes —
the numbers here are correctness verdicts, not performance claims.
