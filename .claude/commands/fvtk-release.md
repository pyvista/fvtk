---
description: Cut an fvtk release — pick the next 9.6.2.N segment, draft release notes from merged PRs, create the GitHub Release that tag-fires trusted publishing for the fvtk wheel and fvtk-sdk.
argument-hint: "[next | 9.6.2.N | rc]"
allowed-tools: Read, Bash, Grep, Glob
---

Cut an fvtk release. The full scheme is `docs/versioning.md`; this is the operational runbook.
fvtk publishes two PyPI projects in lockstep: `fvtk` (runtime wheel, via `ci.yml`, environment
`pypi`) and `fvtk-sdk` (headers + CMake config, via `sdk.yml`, environment `pypi-sdk`). Both use
OIDC trusted publishing — no stored tokens.

Argument: `next` (default, increment the last segment), an explicit `9.6.2.N`, or `rc` for a
pre-release (`9.6.2.Nrc1`).

## The scheme

Version is `9.6.2.N`: the frozen VTK base `9.6.2` plus the fvtk iteration `N`. The version is
**tag-driven** through `setuptools_scm` — there is no version file to edit. A bare `9.6.2` tag is
reserved for an unmodified byte-exact build; fork releases always carry `.N`. Publishing is gated
on a **published GitHub Release**, not a bare `git push --tags`.

## Steps

1. **Confirm `main` is green.** `gh run list --branch main --limit 5`. Do not release off a red
   `main`. Confirm you are releasing the intended commit.
2. **Pick `N`.** List existing tags (`git tag`), find the last published `9.6.2.K`, and take
   `9.6.2.{K+1}` (or the explicit argument). For a pre-release, `9.6.2.{K+1}rc1`.
3. **Draft release notes** from the merged PRs since the last release tag:
   `gh pr list --state merged --base main --search "merged:>=<last-release-date>"`. Group by
   conventional-commit scope (perf, fix, build, ci, test, docs). Lead with parity-relevant
   changes (new default-on optimizations, new opt-in fast filters, backported upstream fixes —
   link their MRs). Run the notes through the prose-hygiene rules in `.claude/CLAUDE.md`: no em
   dashes, no triads, no promo vocab, straight quotes, absolute dates, no AI attribution.
4. **Create the GitHub Release** with the tag `9.6.2.N` targeting the chosen commit. Mark `rc`
   tags as pre-release. This is the single action that triggers publishing; only run it once the
   notes are reviewed.

   ```bash
   gh release create 9.6.2.N --target <sha> --title "fvtk 9.6.2.N" --notes-file <notes> [--prerelease]
   ```
5. **Watch the release workflows.** Publishing the Release fires `ci.yml` (builds the shipped
   LTO abi3 Linux + macOS + Windows wheels, runs the bit-exact and pixel-exact gates, then the
   `publish` job to PyPI `pypi`) and `sdk.yml` (`publish-sdk` to `pypi-sdk`). Watch both:
   `gh run watch`. If a gate fails, the publish does not run — fix forward and re-release; do not
   hand-upload.
6. **Verify** both projects landed at the same version: `pip index versions fvtk` and
   `pip index versions fvtk-sdk` (or check the PyPI project pages). Pin `fvtk` and `fvtk-sdk`
   together.

## First-release note

The first time either project publishes (before it exists on PyPI), a **pending** trusted
publisher must be registered on PyPI first: `fvtk` → workflow `ci.yml`, environment `pypi`;
`fvtk-sdk` → workflow `sdk.yml`, environment `pypi-sdk`. Without it the publish job fails auth.

## Rebasing onto a new VTK

Out of scope for a normal release. When fvtk moves to a new upstream VTK, bump
`VTK_BASE_VERSION` in `ci/cibw/fvtk_backend.py` and the base in `pyproject.toml`, then restart at
`9.7.0.0`. See `docs/versioning.md`.
