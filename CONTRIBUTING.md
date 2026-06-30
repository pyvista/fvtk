Contributing to cvista
====================

**cvista** is a divergent fork of VTK maintained by [PyVista](https://github.com/pyvista),
developed on GitHub at **`pyvista/cvista`** — **not** on the Kitware GitLab, and **not** a
mirror of upstream VTK. Contributions are GitHub pull requests against `pyvista/cvista`.
Read the [README](README.md) first: it is the handoff guide for the build-trim and
swap-for-faster campaigns and explains every lever referenced below.

cvista has one overriding contract: **it is a bit-exact drop-in for stock VTK 9.6.2.** PyVista
runs against cvista unchanged, and the default build is byte-for-byte identical to stock
(`maxULP = 0`) save for the one documented abi3 `type.__flags__` divergence. Everything below
exists to protect that contract while still getting faster.

Rules of engagement (performance work)
--------------------------------------

The fork's whole point is to get faster — but **not at the cost of silently changing what a
filter returns**. Every speedup falls into exactly one of two buckets, decided by whether its
output is byte-identical to stock:

1. **Byte-identical speedup → may be default-on.** If the change produces output that is
   byte-for-byte identical to stock VTK 9.6.2 (`maxULP = 0`, integer arrays width-normalized),
   it can ship enabled by default. This covers devirtualization, LTO/PGO, SIMD that preserves
   rounding (`-ffp-contract=off` on the FMV'd kernels), int32 width-relaxation (values sacred,
   container width negotiable), and *provably* thread-count-invariant parallel loops
   (`out[i] = f(in[i])`, no append/reduction/order-dependent insert — lever 15). The bar is a
   **proof of identity**, not a benchmark: it must pass `tests/bitexact/` at maxULP = 0,
   including the thread-count-determinism check for anything threaded.

2. **Correct but NOT byte-identical → must be opt-in.** If the change is correct but its output
   differs from stock in *any* observable way — cells emitted in a different order, points
   renumbered by a parallel/hash kernel, a vendored algorithm that computes the same answer a
   different way — it **must** be gated behind the explicit `cvista.EnableFast()` opt-in
   (`cvista::FastModeEnabled()` / env `CVISTA_FAST`). The default path stays byte-exact; only
   callers who opt into speed-over-exactness see the divergence. See
   [the EnableFast lane](README.md#opt-in-fast-lane--cvistaenablefast-non-bit-exact-off-by-default)
   for the mechanism (`RunFastFilterParallel` for order-relaxed threading; a `FastModeEnabled()`
   gated adapter for vendored kernels) and the worked examples (cutter, contour, surface,
   clean).

   What "observable difference" admits — and what it never does:
   - **Order is negotiable; positions and values are sacred.** A relaxed result must be the
     *same point set* (coordinates + point-data exact) and the *same cell multiset* (carrying
     the same cell-data) as stock — only emission *order* may differ. Validate with the
     `order_relaxed` / `points_relaxed` modes in `tests/bitexact/`.
   - **Different *values* (even last-ULP) are NOT admissible** under these gates. A kernel that
     changes the math — e.g. area-weighted vs unit-weighted normal averaging, or any
     reduction-order-dependent floating result — is a *different algorithm*, not a reordering,
     and a tolerance gate would mask a real divergence. Don't ship it as a drop-in.
   - **Always prove the kernel actually ran.** A relaxed test passes on the byte-exact fallback
     too, so a green suite alone does not prove the fast path engaged. Every opt-in filter must
     carry an **engagement check** (output order differs from the serial path under
     `EnableFast()`, same set).
   - **Bail to stock for anything you don't handle exactly.** A fast adapter must validate its
     input and return false (→ standard path) for every case outside its proven envelope.

If you can't put a change in bucket 1 with a maxULP = 0 proof, it belongs in bucket 2 behind
the opt-in. There is no third bucket where the default build quietly diverges.

Adding an opt-in fast filter (checklist)
----------------------------------------

1. Gate the fast path on `cvista::FastModeEnabled()`; keep the stock path intact and reachable.
2. For a vendored kernel: keep it in its own non-unity TU, compile under OpenMP behind
   `CVISTA_HAVE_OPENMP` (byte-exact stub without it), preserve the upstream license header, and
   make the adapter fall back (`return false`) for any unsupported input.
3. Add a `tests/bitexact/` op for the new path with the right relaxed flag
   (`order_relaxed` / `points_relaxed`) and coordinate-derived attribute data where canonical
   selection would otherwise be ambiguous.
4. Add an engagement check proving the kernel runs under `EnableFast()`.
5. Confirm the default (fast-off) build stays maxULP = 0 — no regression to existing cases.

Build & validate
----------------

- Build the wheel with the project's cibuildwheel flow (manylinux, single `cp312-abi3`
  wheel); see the README [Building](README.md#building) section. Don't hand-roll a local
  toolchain build for parity claims.
- Run `tests/bitexact/` (stock-vs-cvista dump-and-diff) and, for behavioral changes, the PyVista
  differential suite described in [Parity & validation](README.md#parity--validation). Bar:
  **zero new failures vs stock `vtk` 9.6.2**.

Pull requests
-------------

- Branch from the current development tip; never push to `main`. Stacked PRs are fine (note the
  base and merge order in the description).
- A PR that touches runtime behavior must state which bucket it is in and attach its evidence:
  the maxULP = 0 proof (bucket 1) or the relaxed-gate + engagement result (bucket 2).
- Keep new code in the style of the surrounding file (it is VTK's C++ style); match comment
  density and naming.

Upstream VTK lives at <https://gitlab.kitware.com/vtk/vtk>; cvista does not feed changes back
there and is not affiliated with Kitware.
