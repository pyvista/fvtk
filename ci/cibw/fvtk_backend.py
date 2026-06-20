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
  FVTK_BUILD_DIR        cmake build tree (default <repo>/build-cibw). ONE constant
                        path across the whole cp matrix. A constant path is what
                        makes the cross-leg ccache work: the python-independent C++
                        kit/ThirdParty objects embed the build dir in their -I
                        generated-header paths, so a per-SOABI dir name would change
                        every TU's cache key and force a real C++ recompile on every
                        leg (measured: 0% cross-leg ccache hits when the dir name
                        varies; full hit when constant). By default the tree is
                        wiped between legs (fresh FindPython cache, no ABI bleed)
                        and the C++ comes back via ccache hits.
  FVTK_REUSE_TREE       1 = keep the tree between legs and drop only the ABI-
                        specific wrapper modules + build/lib.* staging, so the
                        surviving *.o skip even the ccache fetch. Faster, slightly
                        riskier (relies on a clean reconfigure); validate per
                        toolchain before enabling in CI.
  FVTK_BUILD_DIR_PER_ABI  1 = key the tree by SOABI (side-by-side per-leg trees);
                        DEFEATS cross-leg ccache — only for debugging one leg.
  FVTK_BUILD_JOBS       cmake --build --parallel N (default: os.cpu_count()).
  FVTK_CMAKE_INIT       init-cache file (default ci/cmake/linux.cmake).
  CMAKE_C/CXX_COMPILER_LAUNCHER  honoured by cmake (e.g. ccache) if exported.
  FVTK_LTO etc.         consumed by the init-cache exactly as in the raw build.
"""
from __future__ import annotations

import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# SINGLE-WHEEL scheme. Python 3.11 has been DROPPED (maintainer directive), so
# the build is ALWAYS abi3: ONE Py_LIMITED_API 0x030c0000 wheel tagged cp312-abi3
# that loads on CPython 3.12+ (incl. future minors). 3.12 is the abi3 floor (and
# now the minimum supported Python): PyMemberDef + the Py_T_*/Py_READONLY member
# constants the heap-type wrappers emit only entered the stable ABI in 3.12
# (gh-93274), so 3.12 is the lowest possible stable-ABI target. cibuildwheel's
# abi3 dedup means the cp312-* cp313-* cp314-* selectors yield ONE build + wheel.
#
# Escape hatch (reversible — re-adds the legacy static path): FVTK_ABI3=0 in the
# env forces a legacy static-type per-version wheel, restoring strict byte-for-byte
# parity incl. __flags__. The single-abi3 default needs the build python to be
# >= 3.12 (which the pyproject `build`/`requires-python` floor guarantees); on an
# unsupported < 3.12 interpreter abi3 cannot apply and the build falls back to the
# legacy static path so a stray cp311 invocation still produces a usable wheel.
ABI3_FLOOR_TAG = "cp312"  # mirrors FVTK_ABI3_VERSION 0x030c0000 in minimal.cmake
ABI3_FLOOR_VERSION = (3, 12)  # mirrors 0x030c0000

# Base version VTK's setup.py.in composes as `{base}.{VTK_VERSION_SUFFIX}`
# (CMake/setup.py.in lines 88-91). Mirrors CMake/vtkVersion.cmake MAJOR.MINOR.BUILD.
VTK_BASE_VERSION = "9.6.2"


def _version_suffix() -> str:
    """Tag-driven ``VTK_VERSION_SUFFIX`` via setuptools_scm (start at post0).

    The PyPI version is driven by git tags: tag ``9.6.2.post0`` -> wheel
    ``9.6.2.post0``; commits past a tag get ``9.6.2.post1.devN`` (never published,
    publish only runs on a release tag). setuptools_scm derives the full PEP 440
    version from the repo's tags (honouring ``SETUPTOOLS_SCM_PRETEND_VERSION_FOR_FVTK``
    if set); VTK's setup.py.in composes ``{VTK_BASE_VERSION}.{VTK_VERSION_SUFFIX}``,
    so we hand it only the suffix (everything past the base + dot).

    Falls back to the historic ``dev0`` suffix when scm/git is unavailable (no
    history, shallow clone, setuptools_scm missing) so non-release builds never
    break. Released wheels are always built on a tag, where the version is exact.
    """
    try:
        from setuptools_scm import get_version

        version = get_version(
            root=REPO,
            dist_name="fvtk",  # so SETUPTOOLS_SCM_PRETEND_VERSION_FOR_FVTK is honoured
            version_scheme="guess-next-dev",
            local_scheme="no-local-version",  # keep dev versions PyPI-upload-clean
            fallback_version=f"{VTK_BASE_VERSION}.post0.dev0",
        )
    except Exception as exc:  # noqa: BLE001 - any scm/git failure -> safe default
        print(
            f"fvtk_backend: setuptools_scm version unavailable ({exc}); "
            f"falling back to VTK_VERSION_SUFFIX=dev0",
            flush=True,
        )
        return "dev0"

    if version == VTK_BASE_VERSION:
        return ""  # tagged exactly 9.6.2 -> official release, no suffix
    prefix = VTK_BASE_VERSION + "."
    if not version.startswith(prefix):
        raise RuntimeError(
            f"fvtk_backend: scm version {version!r} does not start with the VTK "
            f"base {VTK_BASE_VERSION!r}; tag releases as {VTK_BASE_VERSION}.postN "
            f"(e.g. {VTK_BASE_VERSION}.post0)."
        )
    suffix = version[len(prefix):]
    print(f"fvtk_backend: scm version {version} -> VTK_VERSION_SUFFIX={suffix}", flush=True)
    return suffix


def _abi3_enabled() -> bool:
    """Whether THIS build emits the abi3 (stable-ABI) wheel — the default.

    True unless the FVTK_ABI3=0 escape hatch is set, OR the build python is below
    the abi3 floor (3.12) — the stable ABI has no PyMemberDef < 3.12, so a stray
    pre-3.12 interpreter falls back to a legacy static per-version wheel. With the
    Python-3.11 drop the supported floor IS 3.12, so in normal CI this is True."""
    if os.environ.get("FVTK_ABI3", "1") == "0":
        return False
    return sys.version_info[:2] >= ABI3_FLOOR_VERSION


def _build_dir() -> str:
    # SINGLE-WHEEL build tree, a CONSTANT path so the cross-leg ccache hits.
    #
    # The dominant build cost — the python-independent C++ kit + ThirdParty objects
    # (~3000 TUs incl. hdf5 358, exodusII 294, netcdf 100 ...) — is shared across
    # legs ONLY via ccache, and ccache keys each object on its compile command,
    # which embeds the absolute build-dir path of the generated -I include trees.
    # A build-dir name that varies between dedup legs changes every C++ object's
    # cache key and gives 0% hits (measured: varying name -> 0/2 hits; constant
    # name -> full hit on leg 2). So the abi3 wheel gets ONE constant tree:
    #   * abi3 wheel (default): ONE "build-cibw-abi3" tree shared by the cp312/313/
    #     314 dedup legs — the wrappers are version-independent under Py_LIMITED_API,
    #     so cibuildwheel's abi3 dedup means only cp312 builds and 313/314 reuse it.
    #   * legacy static wheel (FVTK_ABI3=0 escape hatch only): an SOABI-keyed tree
    #     (build-cibw-<soabi>), constant for that single per-version leg and kept
    #     distinct so its per-version wrappers don't clobber the abi3 ones.
    # Within each tree the path is constant => cross-leg ccache hits; CCACHE_BASEDIR
    # (fixed below) additionally makes hits survive a different checkout root.
    # FVTK_BUILD_DIR overrides the base location.
    #
    # FVTK_BUILD_DIR_PER_ABI=1 forces a fresh per-SOABI tree even within the abi3
    # group (defeats the cross-leg sharing above; only for debugging one leg).
    base = os.environ.get("FVTK_BUILD_DIR", os.path.join(REPO, "build-cibw"))
    if _abi3_enabled() and os.environ.get("FVTK_BUILD_DIR_PER_ABI") != "1":
        return f"{base}-abi3"
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


def _prepare_tree(build: str) -> None:
    # Reconciling cross-leg ccache (wants a constant build path) with per-leg
    # correctness (the wrappers + the FindPython cache vars are ABI-specific):
    #
    # DEFAULT (safe, proven — mirrors ci/build-wheels-linux.sh): wipe the build
    # tree between legs. A fresh tree means no stale Python3_* cache vars can pin
    # leg N+1 to leg N's interpreter and no ABI-tagged wrapper module can leak into
    # the next wheel. ccache still gives the win: the C++ compile commands are
    # path-identical to the previous leg (constant build dir), so every
    # python-independent kit/ThirdParty TU is a cache HIT — the recompile is just a
    # ccache fetch, not a real compile. This is the cibuildwheel analogue of the
    # raw script's `rm -rf "$BUILD"` + shared /ccache.
    #
    # FVTK_REUSE_TREE=1 (opt-in fast path, validate per-toolchain first): keep the
    # tree and only drop the ABI-specific outputs — the wrapper modules in the
    # top-level package dir (they all land in one constant `fvtk/`, so distinct
    # ABIs WOULD pile up and the wheel's `*-linux-gnu.so` glob would pack them all)
    # plus the per-ABI `build/lib.<plat>` staging trees. The python-independent
    # `*.o` in CMakeFiles/ survive, so ninja skips the C++ recompile entirely
    # (faster than even a ccache fetch). Slightly riskier (relies on CMake cleanly
    # re-deriving every ABI path on reconfigure), hence opt-in.
    if os.environ.get("FVTK_REUSE_TREE") == "1" and os.path.isdir(build):
        for pkg in ("fvtk", "vtkmodules"):
            pkgdir = os.path.join(build, pkg)
            if not os.path.isdir(pkgdir):
                continue
            stale = (
                glob.glob(os.path.join(pkgdir, "*.cpython-*.so"))
                + glob.glob(os.path.join(pkgdir, "*.pyd"))
                + glob.glob(os.path.join(pkgdir, "*-darwin.so"))
            )
            for so in stale:
                try:
                    os.remove(so)
                except OSError:
                    pass
        for libdir in glob.glob(os.path.join(build, "build", "lib.*")):
            shutil.rmtree(libdir, ignore_errors=True)
        return
    # Default: full wipe (fresh cache + no ABI bleed; ccache covers the C++).
    if os.path.isdir(build):
        shutil.rmtree(build, ignore_errors=True)


def _fix_ccache_basedir() -> None:
    # CCACHE_BASEDIR rewrites absolute paths under the repo to relative ones in the
    # compile hash, so an otherwise-identical TU hits across legs / checkouts.
    # pyproject sets it to the literal token "{project}", but cibuildwheel does NOT
    # expand {project} inside environment values (only in before-build/test-command).
    # So the value reaches us un-expanded; resolve it to the real checkout root here.
    # Also covers the un-set case (raw build-fvtk.sh, local runs).
    val = os.environ.get("CCACHE_BASEDIR", "")
    if not val or "{project}" in val:
        os.environ["CCACHE_BASEDIR"] = REPO


def _set_abi3_windows_libdir() -> None:
    # Windows + abi3 (Py_LIMITED_API) link fix, PART 1 of 2 (LNK1104).
    #
    # Under Py_LIMITED_API, MSVC's pyconfig.h emits
    #   #pragma comment(lib, "python3.lib")
    # so every TU including <Python.h> auto-links the BARE name python3.lib (the
    # stable-ABI forwarding stub). The linker resolves a bare-name lib only via
    # its search dirs; FindPython links the version lib by ABSOLUTE path and never
    # adds the CPython libs/ dir to the search path -> LNK1104 on EVERY Python link
    # (WrappingPythonCore — a real VTK module in its own subdir scope — plus every
    # wrapper .pyd).
    #
    # Fix it via the MSVC `LIB` environment variable, which link.exe consults as a
    # default library search path. The build (cmake --build -> ninja -> link.exe)
    # inherits this process env, so the dir reaches every link with NO /LIBPATH on
    # any command line. We avoid putting /LIBPATH into CMAKE_*_LINKER_FLAGS on
    # purpose: ThirdParty/hdf5 embeds the linker flags verbatim into a C string
    # literal (H5build_settings.c), which a quoted Windows path would corrupt.
    # sys.base_prefix/libs is the CPython libs/ dir holding both python3.lib and
    # python3XX.lib (for a venv, base_prefix points at the real install — e.g. the
    # cibuildwheel nuget tools/).
    if os.name != "nt" or not _abi3_enabled():
        return
    libdir = os.path.join(sys.base_prefix, "libs")
    if os.path.isdir(libdir):
        existing = os.environ.get("LIB", "")
        if libdir not in existing.split(os.pathsep):
            os.environ["LIB"] = libdir + (os.pathsep + existing if existing else "")


def _configure_and_build():
    build = _build_dir()
    _fix_ccache_basedir()
    _set_abi3_windows_libdir()
    _prepare_tree(build)
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
        # Keep the backend's abi3 view and cmake's in lockstep: minimal.cmake
        # defaults FVTK_ABI3 ON, and FVTK_ABI3=0 in the env flips both off.
        f"-DFVTK_ABI3={'ON' if _abi3_enabled() else 'OFF'}",
        # Tag-driven PyPI version: setup.py.in builds `{base}.{suffix}`; we drive
        # the suffix from git tags via setuptools_scm (empty for an exact-base tag).
        f"-DVTK_VERSION_SUFFIX={_version_suffix()}",
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


def _retag_abi3(wheel: str) -> str:
    """Rewrite a version-tagged wheel (e.g. ...-cp312-cp312-linux_x86_64.whl) into
    the stable-ABI form ...-cp312-abi3-linux_x86_64.whl: the generated build-tree
    setup.py has no notion of Py_LIMITED_API, so it tags the wheel with the build
    python's version even though the modules are abi3 (vtkXxx.abi3.so). We flip the
    python tag to the floor (cp312), the ABI tag to abi3, rewrite the WHEEL `Tag:`
    line + the RECORD entry for it, and rename the file so the result installs on
    CPython 3.12+. Bit-exact: only the wheel METADATA tag changes, not any module.
    """
    name = os.path.basename(wheel)
    m = re.match(r"^(?P<base>.+)-(?P<py>[^-]+)-(?P<abi>[^-]+)-(?P<plat>[^-]+)\.whl$", name)
    if not m:
        raise RuntimeError(f"cannot parse wheel filename for abi3 retag: {name}")
    if m.group("abi") == "abi3":
        return wheel  # already abi3-tagged
    new_pyabi = f"{ABI3_FLOOR_TAG}-abi3"
    new_name = f"{m.group('base')}-{new_pyabi}-{m.group('plat')}.whl"
    new_path = os.path.join(os.path.dirname(wheel), new_name)

    tmp = wheel + ".retag.tmp"
    with zipfile.ZipFile(wheel, "r") as zin, zipfile.ZipFile(
        tmp, "w", zipfile.ZIP_DEFLATED
    ) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename.endswith(".dist-info/WHEEL"):
                # Replace the whole py-abi-plat triple on every `Tag:` line with
                # the abi3 form (one Tag line in practice; loop is robust to more).
                text = data.decode("utf-8")
                text = re.sub(
                    r"^Tag: .+$",
                    f"Tag: {new_pyabi}-{m.group('plat')}",
                    text,
                    flags=re.MULTILINE,
                )
                data = text.encode("utf-8")
            zout.writestr(item, data)
    os.replace(tmp, new_path)
    if new_path != wheel:
        os.remove(wheel)
    # Fix the RECORD entry for WHEEL (its hash/size changed).
    _rewrite_record_for_wheel(new_path)
    print(f"fvtk_backend: retagged abi3 wheel -> {os.path.basename(new_path)}", flush=True)
    return new_path


def _rewrite_record_for_wheel(wheel: str) -> None:
    """Recompute the RECORD line for the .dist-info/WHEEL file after we edited it."""
    import base64
    import hashlib

    with zipfile.ZipFile(wheel, "r") as z:
        names = z.namelist()
        wheel_name = next(n for n in names if n.endswith(".dist-info/WHEEL"))
        record_name = next(n for n in names if n.endswith(".dist-info/RECORD"))
        wheel_data = z.read(wheel_name)
        record_text = z.read(record_name).decode("utf-8")
        contents = {n: z.read(n) for n in names}

    digest = base64.urlsafe_b64encode(hashlib.sha256(wheel_data).digest()).rstrip(b"=").decode()
    new_line = f"{wheel_name},sha256={digest},{len(wheel_data)}"
    lines = []
    for line in record_text.splitlines():
        if line.startswith(wheel_name + ","):
            lines.append(new_line)
        else:
            lines.append(line)
    contents[record_name] = ("\n".join(lines) + "\n").encode("utf-8")

    tmp = wheel + ".rec.tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for n, data in contents.items():
            z.writestr(n, data)
    os.replace(tmp, wheel)


# --- PEP 517 hooks -----------------------------------------------------------


def get_requires_for_build_wheel(config_settings=None):
    # pip builds the wheel in an ISOLATED env (build isolation), so cmake + ninja
    # must be declared here or the `cmake`/`ninja` binaries are absent from the
    # backend's PATH (CIBW_BEFORE_BUILD installs them into the OUTER python, not
    # pip's isolated build env). pip's cmake>=3.22 wheel ships the binary; ninja
    # >=1.11 is needed for VTK's multiple-output wrapping edges. setuptools+wheel
    # are what the generated build-tree setup.py needs. auditwheel repair is done
    # by cibuildwheel afterwards.
    return ["cmake>=3.22,<4.2", "ninja>=1.11", "setuptools<81", "wheel", "setuptools_scm>=8"]


def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    build = _configure_and_build()
    wheel = _bdist_wheel(build)
    if _abi3_enabled():
        wheel = _retag_abi3(wheel)
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
