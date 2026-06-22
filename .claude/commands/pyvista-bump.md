---
description: Advance the pinned PyVista ref and triage the parity suite — separate real fvtk regressions (fix them) from image-cache drift (deselect them).
argument-hint: "[pyvista-sha-or-tag | latest]"
allowed-tools: Read, Edit, Bash, Grep, Glob
---

Bump the PyVista pin fvtk tests against, run the parity suite, and triage the result. The suite
and its machinery land with PR #112 (`tests/pyvista/`, `ci/run-pyvista.sh`,
`.github/workflows/pyvista-bump.yml`). If `tests/pyvista/` is absent, #112 has not merged yet —
say so and stop.

Argument: a PyVista SHA or tag to pin, or `latest` (default) to take the current tip of
PyVista `main`.

## What the suite is

- `tests/pyvista/PYVISTA_REF` — the pinned PyVista commit. The only thing a bump edits.
- `ci/run-pyvista.sh` — clones PyVista at the pin, installs it `--no-deps` over the built fvtk
  wheel with the `vtkmodules` → `fvtk` redirect, runs core + plotting offscreen under Xvfb +
  Mesa llvmpipe (software GLX, matching PyVista's own CI). Stock `vtk` is never installed, so an
  un-redirected import fails loud.
- `tests/pyvista/deselect.txt` — the triage surface for image-cache drift. `needs_download`
  (network) and snake_case opt-out tests are handled automatically and stay out of it.
- The CI job runs on the merge queue, `workflow_dispatch`, and the `pyvista-full` PR label.
  `pyvista-bump.yml` opens the weekly bump PR automatically; this command is the manual path and
  the triage helper.

## Steps

1. Confirm `tests/pyvista/` exists. Read the current `tests/pyvista/PYVISTA_REF` and
   `tests/pyvista/README.md`.
2. Resolve the target ref. For `latest`, get the current `main` tip:
   `gh api repos/pyvista/pyvista/commits/main --jq .sha`. Show the old → new ref and the
   compare URL.
3. Edit `tests/pyvista/PYVISTA_REF` to the new ref.
4. Run the suite against a built fvtk wheel: `ci/run-pyvista.sh <wheel-dir>` locally if a wheel
   and the Xvfb + Mesa stack are available, otherwise dispatch CI on the branch with
   `gh workflow run ci.yml --ref <branch>` (or push the branch and apply the `pyvista-full`
   label, or just change `PYVISTA_REF`/`deselect.txt` — the `changes` detector triggers the
   gate) and watch it with `gh run watch`.
5. **Triage every failure. This is the real work — distinguish two kinds:**
   - **Image-cache drift** — a plotting test whose rendered image differs from PyVista's
     committed baseline because of a benign rendering difference, not a parity break. These go in
     `tests/pyvista/deselect.txt` with a one-line reason. Only deselect drift; never use it to
     hide a real regression.
   - **Real fvtk regression** — a parity break, a crash, a wrong numerical result, an import
     that should have redirected but did not. **Do not deselect these.** Reproduce against the
     bitexact / renderexact gate, find the fvtk cause, and fix it (or file it and leave the pin
     unbumped). A real regression means fvtk diverged from the contract, which is the whole point
     of the gate.
6. Re-run until green (real fixes applied, drift deselected with reasons).
7. Open the bump PR: title `test(pyvista): bump pin to <short-sha>`, body stating the old → new
   ref, the compare link, what was deselected and why, and any fvtk fix that rode along. Follow
   the commit and prose rules in `.claude/CLAUDE.md`. Do not push to `main`.

If a failure is ambiguous (could be drift or a subtle regression), reproduce it through the
bitexact or renderexact gate before deciding. When still unsure, leave it failing and surface it
rather than deselecting.
