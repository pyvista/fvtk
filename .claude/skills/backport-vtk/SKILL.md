---
name: backport-vtk
description: Sweep recently merged upstream VTK merge requests, triage the bug fixes and features that matter to cvista's shipped modules, and land each as its own PR linked back to the original MR. Use when asked to check upstream VTK for backports, catch cvista up on upstream fixes, or port a specific VTK MR. Drives the vtk-backport-engineer agent for the per-MR landing work.
---

# Backporting upstream VTK fixes into cvista

cvista is frozen on VTK **9.6.2** (upstream tag `v9.6.2`, project `vtk/vtk` id `13` on
`gitlab.kitware.com`). Upstream keeps moving — its `master` is on the 9.7.0 cycle. Real bug
fixes and useful features land upstream after the 9.6.2 cut, and cvista should pull in the ones
that affect the modules it ships. Each backport is **one MR → one PR, linked to the MR**.

cvista's root commit is a squash of 9.6.2, so there is **no shared git history with upstream**.
`git cherry-pick` does not work. A backport applies the MR's diff and adapts it to the 9.6.2
tree by hand where context has drifted.

The reusable details (API calls, diff fetch, triage heuristics) are in
`references/upstream-api.md`. The per-MR landing is delegated to the `vtk-backport-engineer`
agent. The ledger of what has been backported, skipped, or is pending is `docs/upstream-backports.md`.

## The sweep

1. **Read the ledger** `docs/upstream-backports.md`. The watermark is the highest MR iid (or
   the date) already triaged. If the ledger is absent, this is the first sweep: create it and
   use the 9.6.2 tag date as the floor.

2. **Pull merged MRs since the watermark** from the GitLab API (read-only, public). See
   `references/upstream-api.md` for the exact query. Collect iid, title, merge date, changed
   file paths, and web_url for each.

3. **Triage each MR** (heuristics in the reference file; the short version):
   - **Skip** if every changed file is absent from the cvista tree (`git ls-files <path>` empty) —
     that module was trimmed, so the fix is irrelevant. This is the strongest, cheapest filter.
   - **Skip** infra, docs, CI, ThirdParty bumps, deprecation churn, and changes only to modules
     cvista does not ship.
   - **Take** crash / correctness / memory-safety fixes touching shipped modules (highest
     value). Take user-visible feature or behavior fixes case by case.
   - **Defer** performance MRs by default — cvista has its own perf line under the parity
     contract, and an upstream perf change may conflict with it. Flag, do not auto-land.
   - When unsure, mark **pending** in the ledger and surface it for a human call rather than
     guessing.

4. **Present the triage** as a table (iid, title, decision, reason) and confirm scope before
   landing anything. Landing N PRs is a high-volume, outward-facing action; get a go-ahead.

## Landing the accepted backports

For each accepted MR, spawn a `vtk-backport-engineer` agent (run them in parallel for
independent MRs; use worktree isolation so concurrent branches do not collide). Give each agent:

- the MR iid, title, and web_url
- the changed-file list
- the standing instructions below

Each agent, for its one MR:

1. Branch `backport/vtk-mr-<iid>-<short-slug>` from `main`.
2. Fetch the MR diff (`references/upstream-api.md`) and apply it. Try `git apply --3way` first;
   where context has drifted on the 9.6.2 tree, port the change by hand, reading the MR to
   preserve its intent. Keep the change minimal and matched to surrounding VTK style.
3. Handle parity: a behavioral fix diverges from stock 9.6.2 on purpose. Update the affected
   `tests/bitexact/` op to the corrected expectation and note the divergence (see the
   `parity-contract` skill). A crash/guard fix on degenerate input usually leaves valid-input
   output unchanged, so add a regression test for the degenerate case instead.
4. Build (`PROFILE=fast ./build-cvista.sh`) and run the relevant gate (bitexact / renderexact /
   regression) to prove the fix and the parity story.
5. Open the PR. The body **must** link the upstream MR
   (`https://gitlab.kitware.com/vtk/vtk/-/merge_requests/<iid>`), summarize the upstream defect
   in cvista's own words, state the parity bucket, and attach the gate evidence. Commit and PR
   prose follow `.claude/CLAUDE.md` (single-line conventional commit, no AI attribution,
   prose-hygiene pass). Keep it framed around PyVista and VTK; no downstream-consumer rationale.
6. Report back the PR number and the parity outcome.

## Close the loop

Update `docs/upstream-backports.md`: for each MR, record iid, title, decision (`backported
#<PR>` / `skipped: <reason>` / `pending: <reason>` / `deferred: perf`), and the date. Advance
the watermark. The next sweep starts from there, so triage is incremental and never repeats
work.

## Ledger format

```markdown
# Upstream VTK backports

Watermark: MR iid <N> (swept <YYYY-MM-DD>). Base: VTK 9.6.2 (gitlab.kitware.com/vtk/vtk).

| MR | Title | Decision | Date |
|----|-------|----------|------|
| !13353 | vtkPolyLine::Clip nullptr crash on degenerate polylines | backported #120 | 2026-06-21 |
| !13355 | Fix perf issues in marshalling | skipped: SerializationManager not shipped | 2026-06-21 |
```
