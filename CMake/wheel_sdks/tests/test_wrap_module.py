from __future__ import annotations

import os
import re
import sys
from pathlib import Path

import pytest

# Reuse the venv harness from the find_package test (same directory). The
# `virtualenv` fixture there is module-local, so redeclare it here against the
# shared class rather than relying on cross-module fixture discovery.
from test_find_package import VEnv

DIR = Path(__file__).parent.resolve()
BASE = DIR / "packages" / "wrap_module"


@pytest.fixture()
def virtualenv(tmp_path: Path) -> VEnv:
    return VEnv(tmp_path / "venv")


@pytest.mark.skipif(
    sys.platform == "win32",
    reason=(
        "Windows abi3 downstream wrapping needs the stable-ABI import library "
        "(python3.lib) on the link path (Development.SABIModule); the wrap "
        "compiles but the final .pyd link fails to find it in a bare venv. "
        "Proven on Linux and macOS; Windows is a tracked follow-up."
    ),
)
def test_wrap_module(virtualenv: VEnv):
    """Build and Python-wrap an out-of-tree VTK module against the cvista-sdk.

    Installs the `wrap_module` project, which `find_package`s VTK (including the
    WrappingPythonCore component) and drives `vtk_module_scan` +
    `vtk_module_build` + `vtk_module_wrap_python` against the freshly built
    `cvista-sdk` wheel resolved through `--find-links`. A successful install means
    the wrapper C++ compiled against the SDK headers and linked against
    `VTK::WrappingPythonCore`, proving the SDK supports both compiling AND
    Python-wrapping a downstream out-of-tree module end to end.

    This is a build-capability proof, mirroring `test_find_package`: it does not
    import the wrapped module at runtime, which would additionally require the
    matching `cvista` runtime wheel (the SDK ships the VTK libs unvendored for
    linking, not for loading). Instead it asserts the wrapped extension was
    produced inside the installed package.

    It also proves the SDK exports its abi3 (stable-ABI) wrapping setting: the
    SDK's vtk-config re-exports CVISTA_ABI3, so the downstream
    `vtk_module_wrap_python` inherits it and emits a version-INDEPENDENT
    extension (e.g. `SDKExample.abi3.so`) rather than a version-specific
    `SDKExample.cpython-3XX-<plat>.so`. The assertions below require at least one
    `SDKExample*` extension AND that none of them carry a version-specific
    CPython tag, confirming the wrap was abi3.
    """
    virtualenv.run(
        "python", "-m", "pip", "install", "--find-links", os.curdir, str(BASE)
    )

    # The wrapped extension is installed into the `sdk_example` package. Because
    # the SDK exports abi3, it is the version-independent stable-ABI form (e.g.
    # SDKExample.abi3.so / SDKExample.pyd on Windows) rather than a version-
    # specific SDKExample.cpython-3XX-<plat>.so, so glob for it rather than
    # hardcoding the tag (the abi3 suffix differs across platforms).
    pkg = virtualenv.platlib / "sdk_example"
    produced = sorted(
        p.name
        for p in pkg.glob("SDKExample*")
        if p.suffix in {".so", ".pyd"} or ".so" in p.suffixes
    )
    assert produced, (
        f"vtk_module_wrap_python produced no SDKExample extension in {pkg}; "
        f"contents: {sorted(p.name for p in pkg.glob('*')) if pkg.exists() else 'MISSING'}"
    )

    # The wrap must have inherited abi3 from the SDK: no produced extension may
    # carry a version-specific CPython tag (e.g. `cpython-312` / `cp312`). Assert
    # on the ABSENCE of that tag rather than a hardcoded `.abi3.so`, since the
    # abi3 filename suffix differs across platforms.
    version_tag = re.compile(r"cpython-3\d|cp3\d")
    versioned = [name for name in produced if version_tag.search(name)]
    assert not versioned, (
        "vtk_module_wrap_python produced a version-specific extension, so the "
        "wrap did NOT inherit abi3 from the SDK (CVISTA_ABI3 not exported): "
        f"{versioned}"
    )
