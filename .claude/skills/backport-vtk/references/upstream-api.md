# Upstream VTK API and triage reference

Upstream is `vtk/vtk` on `gitlab.kitware.com`, **project id `13`**, default branch `master`.
fvtk is frozen on tag `v9.6.2`. All API access below is read-only and public; no token needed
for public read (an unauthenticated client is rate-limited but fine for a sweep).

## List merged MRs since the watermark

```bash
# By date floor (first sweep: use the 9.6.2 tag date):
curl -s "https://gitlab.kitware.com/api/v4/projects/13/merge_requests\
?state=merged&order_by=updated_at&sort=desc&per_page=100&updated_after=2025-01-01T00:00:00Z"

# Paginate with &page=2, &page=3, … until the result array is empty or you pass the watermark.
```

Each MR object gives: `iid`, `title`, `merged_at`, `web_url`, `labels`, `milestone.title`,
`author.username`, `description`. VTK labels are inconsistent, so **do not rely on labels** for
triage — use the title and the changed files.

## Get the changed files for an MR

```bash
curl -s "https://gitlab.kitware.com/api/v4/projects/13/merge_requests/<iid>/changes" \
  | python3 -c "import sys,json;[print(c['new_path']) for c in json.load(sys.stdin)['changes']]"
```

## Fetch the MR diff to apply

```bash
# Plain unified diff for the whole MR:
curl -sL "https://gitlab.kitware.com/vtk/vtk/-/merge_requests/<iid>.diff" -o /tmp/mr-<iid>.diff

# Apply onto the fvtk tree (no shared history, so 3-way against current paths):
git apply --3way --whitespace=nowarn /tmp/mr-<iid>.diff
```

If `git apply` rejects hunks (the 9.6.2 tree has drifted from upstream master), port the change
by hand: open each rejected file, read the MR's intent from the diff and description, and make
the equivalent edit against the 9.6.2 source. The diff is the guide, not a mechanical patch.

## Triage heuristics

Decide per MR. The cheapest, strongest signal is whether the changed files even exist in fvtk.

1. **Shipped-module filter (run first).** For every changed path, `git ls-files <path>`. If all
   paths are absent from the fvtk tree, the module was trimmed — **skip**. (Cross-check the trim
   lists in `fvtk-config/` if a path is present but the class might be NOWRAP/NOCOMPILE.)

2. **Category from the title and diff:**
   - crash / segfault / nullptr / use-after-free / leak / overflow → **take** (correctness, high
     value).
   - wrong output / incorrect result / regression on a shipped filter or reader → **take**.
   - render / OpenGL / EGL correctness on the shipped rendering stack → **take** (validate with
     renderexact).
   - new feature / API addition on a shipped module → **case by case**; take if PyVista benefits.
   - performance / optimization → **defer** by default. fvtk has its own perf line under the
     parity contract; an upstream perf change may conflict or duplicate. Flag for a human.
   - CI / docs / formatting / ThirdParty version bump / deprecation churn / build infra →
     **skip**.

3. **Parity impact.** Note whether the fix changes valid-input output (a deliberate divergence
   from stock 9.6.2 that needs a bitexact-op update + divergence ledger entry) or only handles
   degenerate input (output unchanged for valid input; add a regression test). See the
   `parity-contract` skill.

4. **When genuinely unsure**, mark `pending` in the ledger and surface it. Do not auto-land an
   ambiguous behavioral change.

## Linking back

Every backport PR body links the MR:

```
Backports https://gitlab.kitware.com/vtk/vtk/-/merge_requests/<iid>
```

This is the provenance record. The ledger `docs/upstream-backports.md` is the index; the PR link
is the per-change trail.
