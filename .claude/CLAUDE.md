# cvista: Claude Code guide

cvista is a trimmed, fast fork of VTK 9.6.2, maintained by the PyVista community as a
drop-in graphics layer for [PyVista](https://github.com/pyvista/pyvista). It is public,
BSD-3 licensed, and developed in the open at `pyvista/cvista`. Read `README.md` for the
public story and `CONTRIBUTING.md` for the rules of engagement.

This file is the always-on context for working in this repo. The skills and commands under
`.claude/` encode the recurring maintenance jobs.

## The prime directive: parity

cvista is a **bit-exact drop-in for stock VTK 9.6.2 by default** (`maxULP = 0`). Every change
falls into one of two buckets:

1. **Byte-identical → ships on by default.** Must pass `tests/bitexact/` at `maxULP = 0`,
   including thread-count determinism for anything threaded.
2. **Correct but reorders output → opt-in behind `cvista.EnableFast()`.** Relaxed-order parity
   gate plus an engagement check proving the fast path ran.

Positions and values are sacred. Only emission order is ever negotiable. Backported upstream
bug fixes are the one deliberate exception: they intentionally diverge from stock 9.6.2 to fix
a real defect, and must be recorded as such. See the `parity-contract` skill before touching
any filter, kernel, or render path.

## Repo map

- `cvista-config/`: the build profiles and the trim lists (`_modules_minimal.cmake`,
  `_nowrap_classes.cmake`, `_nocompile_classes.cmake`). See the `module-trimming` skill.
- `ci/`: build and validation scripts (`build-cvista.sh` entry, `run-bitexact.sh`,
  `run-renderexact.sh`, `run-regression.sh`, `run-pyvista.sh`, `build-sdk.sh`) and the
  cibuildwheel backend under `ci/cibw/`.
- `tests/bitexact/`: the byte-exact gate. `tests/renderexact/`: pixel-exact render gate.
  `tests/regression/`: cvista's own regression scenes. `tests/pyvista/`: the PyVista parity
  suite and its pinned ref (lands with PR #112).
- `docs/`: `build-internals.md` (the lever set and parity gates, read this first for build
  work), `versioning.md`, `abi3-feasibility.md`, `threading-bitexact.md`, `ci-closers.md`,
  `ci-runners.md`.
- `tools/`: `pgo-train.py`, `cvista_shim.py` (the `vtkmodules` → `cvista` import redirect used by
  every parity harness).
- The module trees (`Common/`, `Filters/`, `Rendering/`, `IO/`, …) are the forked VTK source.

## Build and validate

The build runs inside `nix-shell` (pinned cmake 4.1.2, Python 3.13, GL/EGL/OSMesa). Entry:

```bash
./build-cvista.sh                 # default profile (minimal), LTO on
PROFILE=fast ./build-cvista.sh    # fast iteration, LTO off
```

Validation gates (also run in CI):

```bash
ci/run-bitexact.sh      # byte-exact vs stock vtk==9.6.2 (maxULP=0)
ci/run-renderexact.sh   # pixel-exact offscreen render (EGL/Mesa)
ci/run-regression.sh    # cvista's regression scenes
ci/run-pyvista.sh       # PyVista's own suite against the built wheel (PR #112)
```

The parity harnesses install stock `vtk==9.6.2` and the cvista wheel on separate venvs and
redirect `vtkmodules` → `cvista` via `tools/cvista_shim.py`. Never install stock `vtk` into the
cvista venv. Details and env vars are in `tests/bitexact/README.md` and the `parity-contract`
skill.

## Contribution workflow

- Branch from `main`; **never push to `main`**. Every change lands through a pull request.
- One logical change per PR. Backports are one MR per PR (see the `backport-vtk` skill).
- State which parity bucket the change is in, with its evidence, in the PR body.
- CI on `main` is always green. A red branch is the branch's fault; do not wave it off as
  flaky. Run the relevant gate locally before pushing. If main's CI is genuinely failing
  that is an issue that needs to be rectified immediately.

## Commits and PRs

- **Single-line commit messages**, matching the conventional-commit style already in the
  history: `fix(opengl): …`, `perf(SMP): …`, `test(bitexact): …`, `ci: …`, `docs: …`,
  `chore: …`, `build: …`. Pick the scope from the touched area.
- Never `git add -A` or `git add .`; stage explicit paths by name.
- Never `--amend` a pushed commit, never use a HEREDOC commit body.
- **No additional attribution anywhere**: no `Co-Authored-By: Claude`,
  `🤖 Generated with…`, `noreply@anthropic.com`, or `[Claude Code]` in commits, PR bodies,
  PR/issue comments, or release notes.

## Scope discipline

cvista is a community fork for PyVista, and every public artifact stays framed that way: it
exists to give PyVista a graphics layer that is tested, fast, and released on a community
cadence. Keep rationale in commits, PRs, issues, and docs anchored to PyVista and scientific
visualization. Do not reference, name, or speculate about specific downstream commercial users
or private infrastructure in anything that lands in this repo or on GitHub. When a motivation
would only make sense by pointing at a particular downstream consumer, frame it generically
("for large-scale visualization workloads") instead. When unsure, ask.

## Skills and commands

- `/backport-vtk`: sweep recently merged upstream VTK merge requests, triage the ones that
  matter to cvista's shipped modules, and land each as its own PR linked back to the MR.
- `/pyvista-bump`: advance the pinned PyVista ref and triage the parity suite (real
  regression vs image-cache drift).
- `/cvista-release`: cut an cvista release (tag-driven publish to PyPI for the wheel and SDK).
- `/triage-ci`: diagnose a failing CI run or PR check and propose the fix.
- `parity-contract` (skill): the bit-exact contract, the gates, and the fast-filter checklist.
- `module-trimming` (skill): the three trim levers and how to add, remove, or restore classes.
- `vtk-backport-engineer` (agent): lands a single backport end to end; driven by
  `/backport-vtk`.
