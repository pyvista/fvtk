# `.github/` — CI workflows

CI for `fvtk` is mostly VTK C++ compilation. The expensive part is the three
build legs (Linux, macOS, Windows); the numeric/pixel gates are cheap. The build
legs run on [Blacksmith](https://blacksmith.sh) runners when a set of org/repo
variables is defined, and fall back to GitHub-hosted runners when they are not.

## Workflows

| File | Trigger | What it does |
|------|---------|--------------|
| `ci.yml` | PR + push to `main` + release | Build the cp312-abi3 wheel (Linux/macOS/Windows), the bit-exact / pixel-exact / regression gates, the main-only LTO legs, and the release publish. |
| `sdk.yml` | path-scoped PR + `main` + release | Build + publish the `fvtk-sdk` wheel. |
| `wheels-cibuildwheel.yml` | tag + dispatch | Full release wheel matrix. |
| `wheels-legacy.yml` | `main` + dispatch | cp310/cp311 non-abi3 wheels (Linux + Windows). |
| `wheels-test.yml` | dispatch | Manual cross-OS build smoke. |

## Changing the runners (the only knob)

Every `runs-on` reads an Actions **variable** with a GitHub fallback:
`${{ vars.FVTK_RUNNER_* || '<github-default>' }}`. You never edit a workflow to
change runners; you set or clear a variable (Settings → Secrets and variables →
Actions → **Variables**).

| Variable | Set to (Blacksmith) | Unset = fallback |
|----------|---------------------|------------------|
| `FVTK_RUNNER_LINUX`       | `blacksmith-16vcpu-ubuntu-2404`  | `ubuntu-latest` |
| `FVTK_RUNNER_LINUX_SMALL` | `blacksmith-4vcpu-ubuntu-2404`   | `ubuntu-latest` |
| `FVTK_RUNNER_WINDOWS`     | `blacksmith-16vcpu-windows-2025` | `windows-latest` |
| `FVTK_RUNNER_MACOS`       | _(left unset on purpose)_        | `macos-14` |

- **Scale a tier**: edit the value (e.g. `16vcpu` → `32vcpu`). Compile width
  auto-tracks the runner cores — nothing in the repo pins a core count.
- **Roll back a platform**: delete its variable. The leg reverts to the
  GitHub-hosted runner on the next run.
- **Enable macOS later**: set `FVTK_RUNNER_MACOS` (held off today on cost).
- Changes apply on the **next** run; re-run an in-flight PR to pick them up.

Full reference (per-leg mapping, cost notes, caveats) lives in
[`ci/BLACKSMITH.md`](../ci/BLACKSMITH.md).

## CODEOWNERS

Workflow files and the CI docs are owned by `@pyvista/ci-reviewers` (see
[`CODEOWNERS`](./CODEOWNERS)). Changes to them require review from that team. For
the gate to be enforced, branch protection on `main` must have **Require review
from Code Owners** enabled.
