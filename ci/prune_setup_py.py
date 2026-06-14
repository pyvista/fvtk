#!/usr/bin/env python3
"""Prune UI-interactor subpackages from the generated wheel build tree.

PyVista never imports the gtk/qt/tk/wx/test helper subpackages (its Qt path is
the separate pyvistaqt package), but VTK's generated setup.py hardcodes them in
``packages=[...]``. Remove both the directories and the setup.py references so
``setup.py bdist_wheel`` doesn't fail on missing dirs. Mirrors the same step in
build-fvtk.sh.

Usage:  python ci/prune_setup_py.py <build_dir>
"""

import glob
import os
import re
import shutil
import sys

SUBPACKAGES = ("gtk", "qt", "tk", "wx", "test")


def main(build_dir: str) -> int:
    # The package dir is named after the current namespace; handle both the
    # current "vtkmodules" and the future "fvtk" rename.
    pkg_names = ("vtkmodules", "fvtk")
    for pkg in pkg_names:
        for sub in SUBPACKAGES:
            for d in [
                os.path.join(build_dir, pkg, sub),
                *glob.glob(os.path.join(build_dir, "build", "lib.*", pkg, sub)),
            ]:
                if os.path.isdir(d):
                    print(f"prune: rm -rf {d}")
                    shutil.rmtree(d, ignore_errors=True)

    setup_py = os.path.join(build_dir, "setup.py")
    if os.path.isfile(setup_py):
        with open(setup_py, "r", encoding="utf-8") as fh:
            text = fh.read()
        pat = re.compile(
            r"^.*'(?:vtkmodules|fvtk)\.(?:gtk|qt|tk|wx|test)',?.*$\n?",
            re.MULTILINE,
        )
        new_text, n = pat.subn("", text)
        if n:
            with open(setup_py, "w", encoding="utf-8") as fh:
                fh.write(new_text)
            print(f"prune: removed {n} subpackage reference(s) from setup.py")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
