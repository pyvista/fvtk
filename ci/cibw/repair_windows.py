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
import re
import subprocess
import sys
import tempfile
import zipfile


# fvtk no-host-interposition gate (Windows half of ci/check-no-alloc-exports.sh).
# A repaired wheel must not contain any fvtk DLL/.pyd that EXPORTS an allocator
# symbol -- malloc/free/operator new/delete -- or it would interpose the host
# CPython CRT. fvtk routes its own C++ operator new/delete to the single shared
# mimalloc (Common/Core/vtkFVTKAllocator.cxx); on Windows that replacement is
# scoped to the defining DLL (PE/COFF has no cross-DLL interposition), so it must
# never appear in any DLL's export table. The shared mimalloc.dll legitimately
# EXPORTS the mi_* C API (its public interface, consumed across the DLL boundary)
# -- mi_* is NOT the CRT malloc/operator new, so it is allowed.
#
# MSVC-mangled replaceable operator new/delete export names begin with:
#   ??2@  operator new     ??_U@  operator new[]
#   ??3@  operator delete  ??_V@  operator delete[]
# plus the CRT malloc family by name. We use dumpbin /EXPORTS (always on PATH via
# vcvars in the Windows build job).
_WIN_ALLOC_RE = re.compile(
    r"^(malloc|free|calloc|realloc|_aligned_malloc|_aligned_free"
    r"|\?\?2@|\?\?3@|\?\?_U@|\?\?_V@)"
)


def _dumpbin_exports(dll: str) -> list[str]:
    """Exported symbol names of a PE DLL via `dumpbin /EXPORTS` (best-effort)."""
    try:
        out = subprocess.run(
            ["dumpbin", "/EXPORTS", dll],
            capture_output=True,
            text=True,
            check=False,
        ).stdout
    except FileNotFoundError:
        print(
            "repair_windows: WARNING: dumpbin not on PATH; skipping the "
            "no-alloc-exports gate (run under vcvars to enable it).",
            file=sys.stderr,
        )
        return []
    names: list[str] = []
    # dumpbin export rows look like:  "    1    0 00001000 SomeName"
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0].isdigit():
            names.append(parts[-1])
    return names


def _check_no_alloc_exports(wheel: str) -> int:
    """Fail (return 1) if any DLL/.pyd in *wheel* exports an allocator symbol."""
    rc = 0
    scanned = 0
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(wheel) as zf:
            zf.extractall(tmp)
        for dirpath, _dirs, files in os.walk(tmp):
            for name in files:
                if not name.lower().endswith((".dll", ".pyd")):
                    continue
                lib = os.path.join(dirpath, name)
                scanned += 1
                hits = [s for s in _dumpbin_exports(lib) if _WIN_ALLOC_RE.match(s)]
                if hits:
                    print(
                        f"ERROR: {name} EXPORTS allocator symbol(s) "
                        f"(host interposition!): {hits}",
                        file=sys.stderr,
                    )
                    rc = 1
    if rc:
        print(
            "no-alloc-exports gate FAILED: fvtk must not export allocator symbols.",
            file=sys.stderr,
        )
    else:
        print(
            "no-alloc-exports gate OK: no exported malloc/free/operator "
            f"new/delete in {scanned} fvtk DLL(s) (mi_* C API is allowed)."
        )
    return rc


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
    rc = subprocess.call(cmd)
    if rc != 0:
        return rc

    # fvtk no-host-interposition gate on the REPAIRED wheel. delvewheel writes it
    # into dest_dir (it may retag the platform, so find the newest .whl there).
    cands = sorted(
        glob.glob(os.path.join(dest_dir, "*.whl")), key=os.path.getmtime
    )
    if not cands:
        print(
            "repair_windows: WARNING: no repaired wheel found in "
            f"{dest_dir}; skipping the no-alloc-exports gate.",
            file=sys.stderr,
        )
        return 0
    return _check_no_alloc_exports(cands[-1])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
