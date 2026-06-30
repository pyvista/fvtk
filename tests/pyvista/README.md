# PyVista parity gate

Runs **PyVista's own full test suite** against the built cvista wheel, with
`vtkmodules.*` redirected to `cvista.*`. Where `tests/bitexact/` and
`tests/renderexact/` prove cvista is byte- and pixel-identical to stock VTK 9.6.2
on our own scenes, this gate proves the contract a real downstream depends on:
PyVista's thousands of behavioral and plotting assertions pass when PyVista is
driving cvista.

Driver: [`ci/run-pyvista.sh`](../../ci/run-pyvista.sh). CI job: `pyvista` in
[`.github/workflows/ci.yml`](../../.github/workflows/ci.yml).

## How it runs

One venv, no stock `vtk`:

1. Clone PyVista at the pinned SHA in [`PYVISTA_REF`](PYVISTA_REF) (shallow,
   single commit — its committed `image_cache` rides along).
2. `pip install --no-deps ./pyvista` + its `test` dependency-group + the built
   cvista wheel.
3. Drop `tools/cvista_shim.py` as a `.pth` so `import vtkmodules.*` resolves to
   `cvista.*` at interpreter startup. Stock `vtk` is never installed, so any
   un-redirected import fails loud instead of silently testing stock VTK.
4. Run core then plotting as two pytest invocations (matches PyVista's tox
   split), offscreen via Mesa software EGL (llvmpipe), `-n auto`.

Run it locally against a wheel dir:

```bash
ci/run-pyvista.sh /path/to/wheel-dir python3
```

## When it runs (lean by design)

The full suite is heavy, so it is kept **off the PR fast path**. The `pyvista`
job triggers on:

- **`merge_group`** — the merge queue. This is the blocking gate: the suite runs
  on the queued commit before the squash lands, and a red run blocks the merge.
  Mark `pyvista` a **required check for the merge queue** in branch protection.
- **`workflow_dispatch`** — manual run (used while iterating on the harness).
- **`pull_request` labeled `pyvista-full`** — opt-in early signal on a PR.
- **`pull_request` that bumps `PYVISTA_REF`** — auto-detected by the `changes`
  job (a plain git diff), so the pin can never advance unvalidated even without
  the label.

It uses the fast **O2 `build` wheel**, not the LTO wheel: optimization level is
parity-invariant (proven `maxULP=0` by `bitexact` + `bitexact-lto`), so PyVista
returns the same pass/fail on either, and the O2 wheel is already built — the job
never waits on the slow LTO link.

## Pinning and the bump cron

[`PYVISTA_REF`](PYVISTA_REF) holds a pinned PyVista `main` SHA. Floating `main`
would make the gate flaky on upstream churn; pinning makes every run
reproducible.

[`.github/workflows/pyvista-bump.yml`](../../.github/workflows/pyvista-bump.yml)
runs weekly: it resolves PyVista's current `main` HEAD and, if it differs, opens
a PR bumping `PYVISTA_REF` with the `pyvista-full` label. Either the label or the
pin change itself runs the full suite on that PR (and the merge queue runs it on
enqueue), so the pin only advances through a gate that proves cvista still matches
that PyVista revision — including any upstream `image_cache` regeneration.

> Auto-running the suite on the bump PR's checks needs a token that can trigger
> workflows (the default `GITHUB_TOKEN` cannot trigger CI from a bot-opened PR).
> Set repo secret `PYVISTA_BUMP_TOKEN` (a PAT with `contents:write` +
> `pull-requests:write`) for that. Without it, the merge queue still validates the
> bump before it lands — just not as a pre-merge PR check.

## Triage: `deselect.txt`

[`deselect.txt`](deselect.txt) is the deselect list for tests that fail for
reasons that are not a real cvista regression — chiefly **image-regression drift**
(PyVista's committed `image_cache` was generated against a VTK whose pixels
differ from stock 9.6.2). Each entry needs a one-line WHY. Two failure classes
are handled automatically and must **not** be added there:

- `needs_download` network-flaky tests — skipped via `-m "not needs_download"`.
- snake_case opt-out tests — deselected at runtime, but only when the wheel
  disables `VTK_DISABLE_PYTHON_PROPERTIES`.

Every static deselect is a coverage hole. Prefer fixing cvista or bumping the pin
over a permanent entry.
