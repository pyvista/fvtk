"""PEP 517 in-tree build backend that drives fvtk's cmake -> build -> generated
build-tree ``setup.py bdist_wheel`` flow, so ``pip wheel .`` (and therefore
cibuildwheel) can build the wheel.

fvtk is NOT a normal ``pip install .`` project: VTK only emits a ``setup.py``
INSIDE the cmake build tree (VTK_WHEEL_BUILD), after a full C++ configure+build.
This backend bridges that gap:

  build_wheel():
    1. cmake -S <repo> -B <build> -C ci/cmake/linux.cmake  (the proven init-cache)
    2. cmake --build <build> --parallel
    3. ci/prune_setup_py.py <build>   (strip dead UI subpackages)
    4. cd <build> && python setup.py bdist_wheel
    5. copy the produced wheel into the directory pip asked for

It implements only the hooks pip needs to produce a wheel (build_wheel +
get_requires_for_build_wheel + a degenerate build_sdist). No editable install.

Knobs (env):
  FVTK_BUILD_DIR        cmake build tree (default <repo>/build-cibw); kept
                        between python legs so ccache + the configured tree are
                        reused (only the python-version wrappers recompile).
  FVTK_BUILD_JOBS       cmake --build --parallel N (default: os.cpu_count()).
  FVTK_CMAKE_INIT       init-cache file (default ci/cmake/linux.cmake).
  CMAKE_C/CXX_COMPILER_LAUNCHER  honoured by cmake (e.g. ccache) if exported.
  FVTK_LTO etc.         consumed by the init-cache exactly as in the raw build.
"""
from __future__ import annotations

import glob
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _build_dir() -> str:
    # Per-python build tree: the generated wrappers + setup.py are ABI-specific,
    # so cp312 and cp313 must NOT share a configured tree. They DO share the
    # python-independent C++ kit objects via ccache (CMAKE_*_COMPILER_LAUNCHER),
    # so only the first leg pays the full C++ cost. Key the dir by the ABI tag.
    base = os.environ.get("FVTK_BUILD_DIR", os.path.join(REPO, "build-cibw"))
    import sysconfig

    tag = sysconfig.get_config_var("SOABI") or f"py{sys.version_info[0]}{sys.version_info[1]}"
    return f"{base}-{tag}"


def _jobs() -> str:
    return os.environ.get("FVTK_BUILD_JOBS", str(os.cpu_count() or 4))


def _init_cache() -> str:
    # FVTK_CMAKE_INIT selects the per-OS init-cache (ci/cmake/{linux,macos,
    # windows}.cmake). It is set in pyproject [tool.cibuildwheel.<os>.environment]
    # as a REPO-relative path (e.g. "ci/cmake/windows.cmake"): cibuildwheel does
    # NOT expand its {project} token inside environment values (only in
    # before-build/test-command), so an absolute "{project}/..." would reach cmake
    # literally and fail ("Not a file: .../{project}/..."). Resolve a relative
    # value against REPO here; absolute values are passed through unchanged.
    val = os.environ.get("FVTK_CMAKE_INIT")
    if not val:
        return os.path.join(REPO, "ci", "cmake", "linux.cmake")
    return val if os.path.isabs(val) else os.path.join(REPO, val)


def _run(cmd, cwd=None):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=cwd)


def _configure_and_build():
    build = _build_dir()
    launcher_c = os.environ.get("CMAKE_C_COMPILER_LAUNCHER", "")
    launcher_cxx = os.environ.get("CMAKE_CXX_COMPILER_LAUNCHER", "")
    cfg = [
        "cmake",
        "-S",
        REPO,
        "-B",
        build,
        "-G",
        "Ninja",
        "-C",
        _init_cache(),
        f"-DPython3_EXECUTABLE={sys.executable}",
        "-DPython3_FIND_STRATEGY=LOCATION",
    ]
    if launcher_c:
        cfg.append(f"-DCMAKE_C_COMPILER_LAUNCHER={launcher_c}")
    if launcher_cxx:
        cfg.append(f"-DCMAKE_CXX_COMPILER_LAUNCHER={launcher_cxx}")
    _run(cfg)
    _run(["cmake", "--build", build, "--parallel", _jobs()])
    return build


def _bdist_wheel(build: str) -> str:
    _run([sys.executable, os.path.join(REPO, "ci", "prune_setup_py.py"), build])
    # Clean any stale wheel so we can identify the fresh one unambiguously.
    dist = os.path.join(build, "dist")
    if os.path.isdir(dist):
        for w in glob.glob(os.path.join(dist, "*.whl")):
            os.remove(w)
    _run([sys.executable, "setup.py", "bdist_wheel"], cwd=build)
    wheels = glob.glob(os.path.join(dist, "*.whl"))
    if not wheels:
        raise RuntimeError(f"no wheel produced in {dist}")
    return max(wheels, key=os.path.getmtime)


# --- PEP 517 hooks -----------------------------------------------------------


def get_requires_for_build_wheel(config_settings=None):
    # pip builds the wheel in an ISOLATED env (build isolation), so cmake + ninja
    # must be declared here or the `cmake`/`ninja` binaries are absent from the
    # backend's PATH (CIBW_BEFORE_BUILD installs them into the OUTER python, not
    # pip's isolated build env). pip's cmake>=3.22 wheel ships the binary; ninja
    # >=1.11 is needed for VTK's multiple-output wrapping edges. setuptools+wheel
    # are what the generated build-tree setup.py needs. auditwheel repair is done
    # by cibuildwheel afterwards.
    return ["cmake>=3.22,<4.2", "ninja>=1.11", "setuptools<81", "wheel"]


def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    build = _configure_and_build()
    wheel = _bdist_wheel(build)
    dest = os.path.join(wheel_directory, os.path.basename(wheel))
    shutil.copy2(wheel, dest)
    print(f"fvtk_backend: produced {dest}", flush=True)
    return os.path.basename(wheel)


def build_sdist(sdist_directory, config_settings=None):
    raise RuntimeError(
        "fvtk has no sdist: the wheel is generated from the cmake build tree. "
        "Build wheels directly (pip wheel . / cibuildwheel)."
    )


# prepare_metadata_for_build_wheel is intentionally omitted: pip falls back to
# building the full wheel to obtain metadata, which is what we want (the metadata
# only exists after cmake generates setup.py).
