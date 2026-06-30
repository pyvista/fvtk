# Versioning and releases

cvista publishes two PyPI projects — **`cvista`** (the runtime wheel) and
**`cvista-sdk`** (headers + CMake config + import libraries). They share one
version, derived from a single git tag.

## The scheme: `9.6.2.N`

The version is the VTK base this fork is a drop-in for (**`9.6.2`**, frozen) plus
a **4th release segment `N`** — the cvista iteration:

```
9.6.2.0    first cvista release of VTK 9.6.2
9.6.2.1    next cvista release (perf levers, opt-in filters, fixes)
9.6.2.2    ...
```

`N` is a normal [PEP 440](https://peps.python.org/pep-0440/) release component,
incremented every time we ship. The base stays `9.6.2` for the life of the fork
against VTK 9.6.2; it only changes when we rebase onto a new upstream VTK (then
`9.7.0.0`, etc.).

### Why a 4th segment and not `.postN`

cvista ships **real code changes** between releases (devirtualization, LTO/PGO,
opt-in fast filters, threading fixes). PEP 440 reserves `.postN` for
*packaging-only* corrections with **no code changes**, so `.post` would
misdescribe every release as "repackaged VTK 9.6.2." A normal release segment is
honest, and it also keeps the door open for **publishable pre-releases** —
`9.6.2.1rc1` is valid, whereas an RC *of* a post-release is not expressible.

### Special case: bare `9.6.2`

A tag of exactly `9.6.2` (no 4th segment) builds with an empty
`VTK_VERSION_SUFFIX` — reserved for an unmodified, byte-exact official build.
Normal fork releases always carry the `.N` segment.

## How a version is computed

`setuptools_scm` derives the version from git tags (config in
`pyproject.toml` `[tool.setuptools_scm]`; consumed by
`ci/cibw/cvista_backend.py::_version_suffix`, which feeds everything past `9.6.2.`
to VTK's `setup.py.in` as `VTK_VERSION_SUFFIX`). `cvista-sdk` calls the *same*
function (`ci/build-sdk.sh`), so the two wheels always match.

- **On a tag** (`9.6.2.1`) → exact version `9.6.2.1`.
- **Between tags** → `9.6.2.N.devM` (next-iteration dev). These build but are
  **never published** — publish runs only on a release. `local_scheme =
  no-local-version` keeps them PyPI-upload-clean.

## Cutting a release

Publishing is gated on a **published GitHub Release**, not a bare tag push.
A `git push --tags` alone does **not** publish.

1. Pick the next segment `N` (last published `9.6.2.K` → tag `9.6.2.{K+1}`; very
   first release → `9.6.2.0`).
2. Draft a **GitHub Release** in `pyvista/cvista` and create the tag `9.6.2.N` from
   it (target the commit you want to ship).
3. Publishing the Release fires:
   - `.github/workflows/ci.yml` → builds the shipped wheels (Linux LTO abi3 +
     macOS + Windows), runs the bit-exact / pixel-exact gates, then the
     `publish` job (environment **`pypi`**) trusted-publishes `cvista`.
   - `.github/workflows/sdk.yml` → builds + `publish-sdk` (environment
     **`pypi-sdk`**) trusted-publishes `cvista-sdk`.
4. Both use OIDC trusted publishing (no stored tokens). PyPI trusted-publisher
   config:

   | project    | workflow  | environment |
   |------------|-----------|-------------|
   | `cvista`     | `ci.yml`  | `pypi`      |
   | `cvista-sdk` | `sdk.yml` | `pypi-sdk`  |

   For the **first** release of each project (before it exists on PyPI), register
   a **pending** trusted publisher on PyPI with these values.

## Downstream pinning

`9.6.2.N` behaves like any release version — a bare `pip install cvista` always
resolves the latest stable (pre-releases excluded by default):

```
cvista==9.6.2.1        # exact build
cvista==9.6.2.*        # any cvista built against VTK 9.6.2
cvista~=9.6.2.0        # compatible release: >=9.6.2.0, <9.6.3
cvista>=9.6.2.1        # floor
```

Downstream libraries that need a specific fork iteration pin `==9.6.2.N`; those
that only care about the VTK base pin `==9.6.2.*`. Pin `cvista` and `cvista-sdk` to
the same version — they are released in lockstep.

## Rebasing onto a new VTK

When cvista moves to a new upstream VTK (say 9.7.0), bump `VTK_BASE_VERSION`
(`ci/cibw/cvista_backend.py`) and the base in `pyproject.toml`, then restart the
segment at `9.7.0.0`. PEP 440 ordering keeps the lines distinct:
`9.6.2.5 < 9.7.0.0`.
