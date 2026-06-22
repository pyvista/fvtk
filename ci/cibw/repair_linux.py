#!/usr/bin/env python3
"""Linux auditwheel repair wrapper that targets the wheel's OWN architecture.

WHY THIS EXISTS
---------------
cibuildwheel's ``repair-wheel-command`` is a fixed string — it cannot interpolate
the architecture being built. So a hardcoded ``--plat manylinux_2_28_x86_64``
silently assumes x86_64 and makes the repair fail on any other arch with::

    auditwheel repair: error: argument --plat: invalid choice:
        'manylinux_2_28_x86_64' (choose from manylinux_2_39_aarch64, ...)

Derive the platform tag from the wheel's own arch (the pre-repair ``linux_<arch>``
tag in the filename) so one config repairs x86_64 and aarch64 wheels alike. The
glibc floor stays pinned at ``manylinux_2_28`` (the build image), matching the
previous explicit x86_64 tag.

``--strip`` removes local symbol tables (same win as the macOS wrapper); the
dynamic symbols and abi3 ``PyInit_*`` entry points are kept, so the repaired
wheel imports byte-identically. Set ``FVTK_STRIP=0`` to skip the strip pass
(escape hatch, reversible).

USAGE (from pyproject [tool.cibuildwheel.linux] repair-wheel-command):
    python ci/cibw/repair_linux.py {dest_dir} {wheel}

cibuildwheel substitutes ``{dest_dir}`` / ``{wheel}`` and runs with cwd ==
project root.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

# manylinux glibc floor, kept in lockstep with the build image
# ([tool.cibuildwheel.linux] manylinux-*-image in pyproject.toml).
MANYLINUX = "manylinux_2_28"


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit("usage: repair_linux.py <dest_dir> <wheel>")
    dest_dir, wheel = sys.argv[1], sys.argv[2]

    # The pre-repair wheel carries a plain `linux_<arch>` platform tag; auditwheel
    # turns it into `manylinux_2_28_<arch>`. Read the arch from the filename so the
    # --plat we pass matches the wheel we are repairing.
    match = re.search(r"linux_(x86_64|aarch64|ppc64le|s390x|armv7l|i686)\.whl$", os.path.basename(wheel))
    if not match:
        sys.exit(f"repair_linux: cannot determine arch from wheel name {wheel!r}")
    plat = f"{MANYLINUX}_{match.group(1)}"

    cmd = ["auditwheel", "repair", "--plat", plat, "-w", dest_dir, wheel]
    if os.environ.get("FVTK_STRIP", "1") != "0":
        cmd.insert(2, "--strip")
    print("+ " + " ".join(cmd), flush=True)
    subprocess.check_call(cmd)


if __name__ == "__main__":
    main()
