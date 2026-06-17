#!/usr/bin/env python3
"""Windows delvewheel repair wrapper that points delvewheel at fvtk's build-tree
DLL directory.

WHY THIS EXISTS
---------------
fvtk is built with ``VTK_ENABLE_KITS=ON`` (fvtk-config/minimal.cmake): most VTK
modules are folded into a handful of larger "kit" shared libraries (vtkCommon,
vtkFilters, vtkRendering, vtkOpenGL, vtkImaging, vtkInteraction, vtkViews,
vtkParallel, vtkIO). A kit member's per-module DLL is NOT built — its
Python-wrapper ``.pyd`` links the *kit* target (CMake/vtkModule.cmake makes the
per-module real target an INTERFACE lib whose implementation is the kit), so the
``.pyd`` import table names the kit DLL, never a per-module DLL. Modules WITHOUT
a ``KIT`` directive (e.g. ChartsCore) build as their own standalone DLL. Either
way every DLL named in a ``.pyd`` import table is a REAL file that exists in the
build tree.

VTK's wheel layout (CMake/vtkWheelPreparation.cmake) deliberately does NOT copy
those DLLs next to the ``.pyd``s on Windows ("Defaults are fine; handled by
delvewheel") — the DLLs land in ``<build_dir>/bin`` (CMakeLists.txt:277,
``CMAKE_RUNTIME_OUTPUT_DIRECTORY = <build>/${CMAKE_INSTALL_BINDIR}``), and
``delvewheel repair`` is expected to find them and bundle them into the wheel.

In the cibuildwheel flow that build ``bin`` dir is not on PATH when delvewheel
runs, so delvewheel reports the first DLL it can't resolve
(``Unable to find library: vtkchartscore-9.6.2.dll``). This is purely a SEARCH
PATH problem — NOT a kit/per-module name mismatch — so ``--add-path <bin>`` over
the build tree resolves ALL of them (kit DLLs + standalone module DLLs +
vendored third-party DLLs) in one shot.

The fvtk backend (ci/cibw/fvtk_backend.py) keys each python leg's build tree by
SOABI as ``build-cibw-<SOABI>``; this wrapper globs every ``build-cibw*/bin``
under the project root (plus a couple of fallbacks) and passes them all to
delvewheel via repeated ``--add-path``.

USAGE (from pyproject [tool.cibuildwheel.windows] repair-wheel-command):
    python ci/cibw/repair_windows.py . {dest_dir} {wheel}

cibuildwheel does NOT substitute ``{project}`` in repair-wheel-command (only
``{wheel}``/``{dest_dir}``), but it runs the command with cwd == the project
root, so the command passes ``.`` as the project arg and this script resolves it
to an absolute path (and defensively falls back to cwd if an unsubstituted
``{project}`` placeholder ever reaches it).
"""
from __future__ import annotations

import glob
import os
import subprocess
import sys


def _bin_dirs(project: str) -> list[str]:
    """Every build-tree directory that may hold fvtk's runtime DLLs."""
    patterns = [
        os.path.join(project, "build-cibw*", "bin"),
        # FVTK_BUILD_DIR override (if ever set) lands elsewhere; also honour it.
        os.path.join(os.environ.get("FVTK_BUILD_DIR", ""), "*", "bin")
        if os.environ.get("FVTK_BUILD_DIR")
        else "",
    ]
    found: list[str] = []
    for pat in patterns:
        if not pat:
            continue
        for d in glob.glob(pat):
            if os.path.isdir(d) and d not in found:
                found.append(d)
    return found


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(
            "usage: repair_windows.py <project> <dest_dir> <wheel>",
            file=sys.stderr,
        )
        return 2
    project, dest_dir, wheel = argv[0], argv[1], argv[2]
    # cibuildwheel runs repair-wheel-command with cwd == project root and does
    # not expand {project}; "." (or a stray unsubstituted "{project}") resolves
    # to that cwd.
    if not project or "{" in project:
        project = os.getcwd()
    project = os.path.abspath(project)

    bin_dirs = _bin_dirs(project)
    if not bin_dirs:
        print(
            f"repair_windows: WARNING: no build-tree bin dir found under {project}; "
            "delvewheel will rely on PATH only.",
            file=sys.stderr,
        )

    cmd = ["delvewheel", "repair", "-w", dest_dir]
    for d in bin_dirs:
        cmd += ["--add-path", d]
    cmd.append(wheel)

    print("repair_windows: build DLL dirs:", flush=True)
    for d in bin_dirs:
        print(f"  - {d}", flush=True)
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
