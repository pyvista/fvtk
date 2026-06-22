from __future__ import annotations

import os
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


def test_wrap_module(virtualenv: VEnv):
    """Build and Python-wrap an out-of-tree VTK module against the fvtk-sdk.

    Installs the `wrap_module` project, which `find_package`s VTK (including the
    WrappingPythonCore component) and drives `vtk_module_scan` +
    `vtk_module_build` + `vtk_module_wrap_python` against the freshly built
    `fvtk-sdk` wheel resolved through `--find-links`. A successful install means
    the wrapper C++ compiled against the SDK headers and linked against
    `VTK::WrappingPythonCore`, proving the SDK supports both compiling AND
    Python-wrapping a downstream out-of-tree module end to end.

    This is a build-capability proof, mirroring `test_find_package`: it does not
    import the wrapped module at runtime, which would additionally require the
    matching `fvtk` runtime wheel (the SDK ships the VTK libs unvendored for
    linking, not for loading). Instead it asserts the wrapped extension was
    produced inside the installed package.
    """
    virtualenv.run(
        "python", "-m", "pip", "install", "--find-links", os.curdir, str(BASE)
    )

    # The wrapped extension is installed into the `sdk_example` package. Its
    # filename carries the interpreter SOABI tag (e.g. SDKExample.cpython-3XX-
    # <plat>.so / SDKExample.pyd), so glob for it rather than hardcoding the tag.
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
