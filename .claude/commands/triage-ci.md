---
description: Diagnose a failing fvtk CI run or PR check — fetch the failing job logs, classify the failure against the known gates, and propose or apply the fix.
argument-hint: "[PR# | run-id | branch]"
allowed-tools: Read, Bash, Grep, Glob, Edit
---

Diagnose a failing CI run and get to the fix. The gate layout is in `docs/ci-closers.md` and
`docs/ci-runners.md`; this command maps a red check to its cause.

Argument: a PR number, a run id, or a branch. Default to the current branch's latest run.

## Pull the failure

```bash
gh pr checks <PR#>                       # which checks are red
gh run list --branch <branch> --limit 5
gh run view <run-id> --log-failed        # the failing job's log tail
```

Read the failing job's log, not just its name. Identify which gate failed and the first real
error (not the cascade after it).

## Classify against the gates

`ci.yml` runs these job groups. Match the red one:

- **build (Linux / macOS / Windows cibuildwheel)** — compile, link, codegen, wheel-repair, or
  smoke failure. Common causes: a missing class a kept path references (a trim closure broke —
  see the `module-trimming` skill), a configure-time module cascade, a platform-specific compile
  error (MSVC vcvars, AppleClang), an `abi3` / `python3.lib` link issue on Windows. The smoke
  step (`ci/smoke_test.py` / `smoke_min.py`) failing means the wheel imports or renders wrong.

- **bitexact** — output diverged from stock VTK 9.6.2 (`maxULP > 0`). This is a **parity
  break**. A change that was meant to be byte-identical is not. Either make it byte-identical
  again, move it behind `fvtk.EnableFast()` as a Bucket-2 opt-in, or — if it is an intentional
  backport divergence — update the affected op and record it in the divergence ledger. See the
  `parity-contract` skill. Never relax the comparison to make it pass.

- **renderexact** — offscreen RGBA+Z framebuffer diverged. A render-path change broke pixel
  exactness. Reproduce with `ci/run-renderexact.sh` under software EGL; treat like a bitexact
  break.

- **regression** — an fvtk regression scene failed. Read the specific test; it usually points at
  a concrete filter or render behavior.

- **pyvista** (PR #112, merge-queue / `pyvista-full` label) — PyVista's own suite failed against
  the built wheel. Split image-cache drift from a real fvtk regression exactly as the
  `/pyvista-bump` command describes. Real regression → fix in fvtk; drift → `deselect.txt` with a
  reason. Never deselect a real regression.

- **\*-lto legs** (main only) — the shipped LTO+ICF config failed to build or validate. The
  fast-O2 PR build passed but the LTO build did not. Usually an LTO/ICF-sensitive construct or a
  link-time issue; check the LTO build log specifically.

- **sdk / sdk-gate** — the `fvtk-sdk` build or its external-project consumer test failed (the SDK
  ships headers + CMake config + import libs). Check `ci/build-sdk.sh` / `ci/test-sdk.sh` and the
  per-platform link setup (Windows `python3.lib`, macOS).

- **publish** (release only) — runs only on a published GitHub Release; a failure here is
  trusted-publisher / environment config, not code. See `/fvtk-release`.

## Reproduce, then fix

Reproduce locally before changing anything: `PROFILE=fast ./build-fvtk.sh` plus the matching
gate script (`ci/run-bitexact.sh`, `ci/run-renderexact.sh`, `ci/run-regression.sh`,
`ci/run-pyvista.sh`). A gate that fails in CI but not locally is usually environment (runner
arch, Mesa version, contention), not flakiness — `docs/ci-runners.md` covers the runner matrix.
CI on `main` is always green; a red branch is the branch's fault.

Propose the fix with its root cause and the gate evidence. Apply it only on a branch, follow the
commit and prose rules in `.claude/CLAUDE.md`, and re-run the failed gate to confirm before
handing back.
