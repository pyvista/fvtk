<p align="center">
  <img src="https://github.com/pyvista/pyvista/raw/main/doc/source/_static/pyvista_logo.svg" alt="PyVista" width="400" />
</p>

<p align="center">
  <strong>fvtk: a fast, lean fork of VTK, maintained for the PyVista community</strong>
</p>

<p align="center">
  <a href="https://pypi.org/project/fvtk/"><img src="https://img.shields.io/pypi/v/fvtk.svg?logo=python&logoColor=white" alt="PyPI" /></a>
  <a href="https://pypi.org/project/fvtk-sdk/"><img src="https://img.shields.io/pypi/v/fvtk-sdk.svg?label=fvtk-sdk&logo=python&logoColor=white" alt="fvtk-sdk PyPI" /></a>
  <a href="https://pypi.org/project/fvtk/"><img src="https://img.shields.io/badge/ABI-abi3%20(CPython%203.12%2B)-orange?logo=python&logoColor=white" alt="Stable ABI (abi3), CPython 3.12+" /></a>
  <a href="Copyright.txt"><img src="https://img.shields.io/badge/License-BSD%203--Clause-yellow.svg" alt="BSD 3-Clause License" /></a>
  <a href="https://gitlab.kitware.com/vtk/vtk/-/tags/v9.6.2"><img src="https://img.shields.io/badge/fork%20of-VTK%209.6.2-blue.svg" alt="Fork of VTK 9.6.2" /></a>
</p>

<p align="center">
  <em>
    fvtk is an open, community-maintained graphics layer for
    <a href="https://github.com/pyvista/pyvista">PyVista</a>. It is BSD-3 licensed,
    developed in the open on <a href="https://github.com/pyvista/fvtk">pyvista/fvtk</a>,
    and not affiliated with Kitware.
  </em>
</p>

**fvtk** (the "f" is for *fast*) is a fork of [VTK](https://gitlab.kitware.com/vtk/vtk)
maintained by the [PyVista](https://github.com/pyvista) organization. It ships the same
visualization toolkit PyVista already runs on, packaged to be smaller, faster, and released
on a cadence the community sets.

fvtk gives PyVista users:

- a drop-in replacement for the VTK Python wheel, byte-for-byte identical by default
- a wheel that is roughly a third the size of stock VTK (~37 MB vs ~120 MB)
- faster filters, with more being replaced over time
- a graphics layer that is tested against PyVista on every change
- releases when the community needs them, on every platform the community runs

## Why fvtk exists

PyVista is built on VTK, and it would not exist without it. VTK carries three decades of
visualization algorithms, and Kitware deserves real credit for that body of work. But VTK
and PyVista are run by different people with different priorities, and keeping PyVista
compatible with VTK has grown into a recurring tax that the PyVista community pays alone.

A few patterns drove the decision to fork:

- **Downstream breakage is routine.** VTK does not test against PyVista, or against any
  other downstream consumer. Each VTK release lands behavior changes that PyVista discovers
  in its own CI after the fact, and the PyVista maintainers file the regressions release by
  release. Asking for contracts or tests around the APIs the community depends on has not
  moved the needle.
- **Contributions stall.** PyVista has sent fixes upstream for years. Many of them sit
  unmerged. The path from "a downstream library found and fixed a bug" to "the fix ships in
  VTK" is long enough that PyVista routinely ships workarounds instead.
- **Packaging follows Kitware's priorities, not the community's.** Platform coverage and
  release timing track what Kitware is resourced to do. ARM64 Linux wheels, for one example,
  arrived long after the hardware was mainstream. The people who depend on VTK have had little
  say in any of it or are told they need to fork up money to Kitware to get any level of basic
  support.
- **Performance has become a ceiling.** For a growing set of filters, VTK's implementation is
  the bottleneck in real PyVista workloads. We are replacing those algorithms with faster ones,
  often with speedups over 50x (we are slowly releasing these in public over time).

Open source is more than a license. A project is only as open as its willingness to engage
the people building on top of it: to test the contract downstream depends on, to merge the
fixes they send, and to ship on a cadence the community can plan around. fvtk is the PyVista
community taking ownership of the graphics layer it depends on, so that PyVista has a base
that is tested, fast, and stewarded by the people who actually use it.

This is not a hostile fork. fvtk respects VTK's BSD license, records its upstream provenance,
and stays a faithful drop-in by default. It exists to serve PyVista well, which is something
the community can do for itself.

## What you get

fvtk has two phases. Today it is a trimmed, drop-in VTK. Over time it diverges as we replace
components with faster implementations.

| | fvtk | stock `vtk` 9.6.2 |
|---|---|---|
| Wheel size (stripped) | **~37 MB** | ~120 MB |
| Filter benchmark | **~2 % faster** out of the box, ~26 % with opt-in PGO | reference |
| Modules shipped | ~84 (PyVista's closure) | ~160 |
| Python support | one stable-ABI `cp312-abi3` wheel, CPython 3.12+ | per-minor wheels |
| Default behavior | byte-for-byte identical (`maxULP = 0`) | reference |
| Import name | `fvtk` | `vtkmodules` |

**Phase 1, today: a trimmed drop-in.** fvtk ships only the modules PyVista imports (core,
filters, IO, and the full rendering stack) and their dependencies. It installs as the `fvtk`
package rather than `vtk`, so it coexists with a stock VTK install instead of clobbering it.
The default build is byte-for-byte identical to VTK 9.6.2, verified down to `maxULP = 0`
against a bit-exact suite and PyVista's full test suite. One documented exception exists
(`type(x).__flags__` differs because the stable-ABI wrappers are heap types). PyVista runs
unchanged once it is taught to import `fvtk`.

**Phase 2, ongoing: faster.** Individual VTK components are replaced with faster
implementations, and dead code is removed. Speedups that stay byte-identical to stock ship on
by default (LTO, AVX2 on the hot vertical kernels, multithreading on filters that are provably
deterministic). Speedups that are correct but reorder output are gated behind an explicit
`fvtk.EnableFast()` opt-in, so the default build stays an exact drop-in. The contract is
strict: positions and values are sacred, only emission order is ever negotiable, and every
change carries a proof. See [build internals](docs/build-internals.md) for the full set of
levers and the parity gates that protect them.

## Install

```bash
pip install fvtk
```

fvtk ships a single stable-ABI wheel for CPython 3.12 and newer, including future minors with
no rebuild.

## Using fvtk with PyVista

PyVista imports `vtkmodules`, so it needs to be told to import `fvtk`. There are two paths.

The quick path is an import shim that aliases `vtkmodules` to `fvtk` before PyVista loads,
which needs no PyVista changes:

```python
import importlib.util, sys, fvtk

class _FvtkShim:
    def find_spec(self, name, path=None, target=None):
        if name == "vtkmodules" or name.startswith("vtkmodules."):
            return importlib.util.find_spec("fvtk" + name[len("vtkmodules"):])
        return None

sys.meta_path.insert(0, _FvtkShim())
import pyvista  # now resolves vtkmodules.* against fvtk.*
```

The intended end state is a PyVista backend selector (for example a `PYVISTA_VTK_BACKEND=fvtk`
switch) that imports `fvtk` directly. That work lands in PyVista itself. See the
[namespace section](docs/build-internals.md#namespace--coexists-with-stock-vtk) for details.

## Modules beyond the core

fvtk's core ships lean on purpose. It carries the module closure PyVista actually uses, and
nothing else. VTK's other modules (the larger IO format readers, info-vis, and
the rest) are not gone from the world. They belong in their own repositories.

The model is decentralization:

- **fvtk is the base.** It builds and publishes both a runtime wheel and a development **SDK**
  (headers, CMake config, and import libraries). The SDK is what everything else builds
  against, the same way external projects build against an installed VTK via
  `find_package(VTK)`.
- **Other modules live in their own repos.** A VTK module that fvtk does not ship can be
  maintained as a small, focused package that builds against the fvtk SDK. It depends on
  fvtk, releases on its own schedule, and is owned by the people who care about it.
- **The stack grows by composition.** Instead of one monolith that releases on a single
  cadence and gates every contribution through one maintainer team, the stack becomes a
  base layer plus independent modules, each moving at its own speed.

This keeps the base small, fast, and easy to release, and it lets domain experts own and ship
the pieces they use without waiting on anyone. If you maintain a VTK module that PyVista's
community relies on, building it against the fvtk SDK is the path to a release cadence you
control.

## Contributing

fvtk is for the PyVista community, and it is built by the PyVista community. Contributions of
every size are welcome: bug reports, faster filter implementations, packaging and CI work,
documentation, or a new module repo built on the SDK.

The one rule that governs everything is the parity contract: **fvtk is a bit-exact drop-in for
VTK 9.6.2 by default.** Every speedup falls into one of two buckets, and the right bucket is
decided by whether its output is byte-identical to stock:

1. **Byte-identical → can ship on by default.** It must pass `tests/bitexact/` at
   `maxULP = 0`, including a thread-count-determinism check for anything threaded.
2. **Correct but reorders output → must be opt-in.** It goes behind `fvtk.EnableFast()`, with
   a relaxed-order parity gate and an engagement check proving the fast path actually ran.

Positions and values are always sacred. Only emission order is ever negotiable, and a kernel
that changes the math (even by a last ULP) is a different algorithm, not a reordering. The
full rules, the opt-in checklist, and the validation flow are in
[`CONTRIBUTING.md`](CONTRIBUTING.md).

Development happens on [`pyvista/fvtk`](https://github.com/pyvista/fvtk) via GitHub pull
requests. Branch from the development tip, never push to `main`, and state which parity bucket
your change is in with its evidence attached.

## Roadmap

- **Wheels everywhere.** A cibuildwheel matrix mirroring PyVista's support set across Linux
  x86-64, Linux ARM64, macOS, and Windows, built inside `manylinux` images.
- **A published SDK.** Ship the development SDK (headers, CMake config, import libraries)
  alongside the runtime wheel so external module repos can build against fvtk directly.
- **First-class PyVista support.** A PyVista backend selector that imports fvtk natively,
  replacing the import shim.
- **More speed.** Continue replacing hot VTK components with faster implementations under the
  parity contract. This is the phase where fvtk earns the "f".
- **Spin out the modules.** Help move the stripped VTK modules into their own repositories
  built on the SDK, so the stack decentralizes and each piece releases on its own cadence.

## Relationship to VTK and Kitware

fvtk is a fork of VTK 9.6.2, distributed under VTK's own OSI-approved BSD 3-Clause license
(see [`Copyright.txt`](Copyright.txt)). Upstream provenance is recorded in the root commit.
fvtk is not affiliated with or endorsed by Kitware, does not feed changes back to the Kitware's
self-hosted GitLab, and is not a mirror of upstream VTK. For the original project, see
[vtk.org](https://www.vtk.org/).

We hold no ill will toward the engineers behind VTK. The fork exists because the PyVista
community needs a graphics layer it can test, fix, and release on its own terms, and that is a
thing the community is free to build for itself under the license VTK ships.

## License

fvtk inherits VTK's BSD 3-Clause license; see [`Copyright.txt`](Copyright.txt). For build
internals, the lever set, and the parity gates, see
[`docs/build-internals.md`](docs/build-internals.md).
