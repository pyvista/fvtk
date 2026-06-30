# Blacksmith CI runners

The CI build legs (the only expensive part — all VTK C++ compilation) run on
[Blacksmith](https://blacksmith.sh) runners when a set of org/repo **variables**
is defined, and fall back to GitHub-hosted runners when they are not. There is
**no hardcoded core count** anywhere: the compile width auto-tracks the runner's
vCPUs (`cvista_backend._jobs()` defaults `cmake --build --parallel` to
`os.cpu_count()`; `pyproject.toml` no longer pins `CVISTA_BUILD_JOBS`). Scaling a
tier up or down — or rolling back to GitHub — is a one-variable change, never a
workflow edit.

## The variables

Set these as **organization** variables (Settings → Secrets and variables →
Actions → Variables) so every PyVista repo shares them, or as repo variables to
scope to `cvista`. Unset = the GitHub-hosted fallback in parentheses.

| Variable | Heavy / cheap | Suggested value | Fallback |
|----------|---------------|-----------------|----------|
| `CVISTA_RUNNER_LINUX`       | heavy compile + LTO link | `blacksmith-16vcpu-ubuntu-2404` | `ubuntu-latest` |
| `CVISTA_RUNNER_LINUX_SMALL` | short test gates         | `blacksmith-4vcpu-ubuntu-2404`  | `ubuntu-latest` |
| `CVISTA_RUNNER_WINDOWS`     | Windows compile          | `blacksmith-16vcpu-windows-2025` | `windows-latest` |
| `CVISTA_RUNNER_MACOS`       | macOS compile — **leave UNSET for now** | _(see note)_ | `macos-14` |

> **macOS is intentionally NOT enabled.** The macOS legs are fully wired to read
> `CVISTA_RUNNER_MACOS`, but the variable is **deliberately left unset** so macOS
> keeps running on the free GitHub `macos-14` runner. The Blacksmith macOS tier
> (~$0.16/min on 12-vcpu M4) was judged too expensive for the per-run cost. To
> turn it on later, just set `CVISTA_RUNNER_MACOS` (e.g.
> `blacksmith-12vcpu-macos-latest`, or `blacksmith-6vcpu-macos-latest` at
> $0.08/min as a cheaper middle ground) — no workflow edit needed.

`vars.X || 'fallback'` means: any variable left unset transparently uses the
GitHub-hosted runner. This is also the kill switch — clear `CVISTA_RUNNER_LINUX`
org-wide and every Linux build leg reverts to GitHub on the next run (Blacksmith
has had availability incidents; the fallback is deliberate).

## Where each variable is used

- `CVISTA_RUNNER_LINUX` — `ci.yml` (`build`, `build-lto`, `build-legacy-linux`),
  `sdk.yml` (`build-sdk`), `wheels-cibuildwheel.yml` (linux leg),
  `wheels-test.yml` (linux leg).
- `CVISTA_RUNNER_WINDOWS` — `ci.yml` (`build-other-os`, `build-legacy-windows`),
  the wheels workflows' Windows legs.
- `CVISTA_RUNNER_MACOS` — `ci.yml` (`build-other-os`, `build-legacy-macos`), the
  wheels workflows' macOS legs.
- `CVISTA_RUNNER_LINUX_SMALL` — `ci.yml` gates (`bitexact`, `regression`,
  `renderexact`, `*-lto`, `publish`), `sdk.yml` (`publish-sdk`).

## Notes / caveats

- **Public repo = free GitHub runners today.** `pyvista/cvista` is public, so the
  GitHub-hosted standard runners cost nothing. Blacksmith spend is net-new,
  bought for *faster PR turnaround*, not for a cost saving.
- **macOS stays on free GitHub for now** (the chosen rollout): set
  `CVISTA_RUNNER_LINUX` / `CVISTA_RUNNER_WINDOWS`, leave `CVISTA_RUNNER_MACOS` unset.
  macOS is the pricey leg (~$0.16/min, 12-vcpu M4) and is held off until the
  cost is justified — see the macOS note above.
- **Windows Server 2025 runners are Public Beta** — flip via the variable, watch
  the first runs, keep `windows-latest` as the fallback.
- **Want oversubscription instead of 1 job/core?** Set `CVISTA_BUILD_JOBS` (env or
  variable) in the heavy build step; unset tracks the runner cores.
- Blacksmith transparently accelerates `actions/cache`, so the existing ccache
  cross-run seed logic keeps working with no change.
