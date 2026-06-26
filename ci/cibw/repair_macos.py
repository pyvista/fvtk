#!/usr/bin/env python3
"""macOS delocate repair wrapper that ALSO strips local symbol tables.

WHY THIS EXISTS
---------------
cibuildwheel's default macOS repair is just ``delocate-wheel`` — it bundles the
vendored dylibs into the wheel but does NOT strip symbols. cvista's ``.so`` modules
and the dylibs delocate copies in carry large local-symbol tables (every static
function, every internal C++ symbol). They are dead weight at import time: the
dynamic loader resolves imports through the *dynamic* symbol table only, so the
local entries can be dropped with zero behavioural change.

``strip -x`` removes the local symbols while KEEPING every externally-visible
(global/dynamic) symbol — so the abi3 ``PyInit_*`` entry points and cvista's own
cross-module runtime symbols (``PyVTKObject_New``, ``PyVTKEnum_Add``, … shared
between cvista's ``.so`` files) survive. This mirrors the Linux ``auditwheel repair
--strip`` win (see pyproject ``[tool.cibuildwheel.linux]``), which measured ~15%
off the wheel.

THE APPLE-SILICON CATCH
-----------------------
On arm64 every Mach-O carries a (possibly ad-hoc) code signature, and the kernel
REFUSES to load a binary whose contents no longer match its signature. ``strip``
rewrites the file and thus invalidates the signature, so each stripped binary
MUST be re-signed. We re-sign ad-hoc (``codesign --force --sign -``) exactly as
delocate itself does after it rewrites load commands — no identity/keychain
needed, and pip/import do not verify a signing authority, only that the signature
is INTERNALLY consistent.

ORDER MATTERS: run ``delocate-wheel`` FIRST (it copies + re-signs the dylibs),
THEN strip + re-sign. Stripping before delocate would just be undone by
delocate's own rewrite.

USAGE (from pyproject [tool.cibuildwheel.macos] repair-wheel-command):
    python ci/cibw/repair_macos.py {delocate_archs} {dest_dir} {wheel}

cibuildwheel substitutes ``{delocate_archs}`` / ``{dest_dir}`` / ``{wheel}`` and
runs with cwd == project root. Set ``CVISTA_STRIP=0`` to skip the strip pass and
fall back to a plain delocate repair (escape hatch, reversible).
"""
from __future__ import annotations

import base64
import csv
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile


def _run(cmd: list[str]) -> None:
    print("+ " + " ".join(cmd), flush=True)
    subprocess.check_call(cmd)


def _is_macho(path: str) -> bool:
    """True if *path* is a Mach-O (thin or fat) binary we should strip."""
    try:
        with open(path, "rb") as fh:
            magic = fh.read(4)
    except OSError:
        return False
    # thin LE/BE 32/64 (cffaedfe/feedface/cffaedfe/feedfacf) + fat (cafebabe/bebafeca)
    return magic in (
        b"\xcf\xfa\xed\xfe",  # MH_MAGIC_64 LE
        b"\xce\xfa\xed\xfe",  # MH_MAGIC LE
        b"\xfe\xed\xfa\xcf",  # MH_MAGIC_64 BE
        b"\xfe\xed\xfa\xce",  # MH_MAGIC BE
        b"\xca\xfe\xba\xbe",  # FAT_MAGIC
        b"\xbe\xba\xfe\xca",  # FAT_CIGAM
    )


def _strip_and_sign(root: str) -> int:
    """Strip local symbols from every Mach-O under *root* and re-sign ad-hoc.

    Returns the number of bytes saved (sum of before-minus-after sizes)."""
    saved = 0
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            if not (name.endswith(".so") or name.endswith(".dylib")):
                continue
            p = os.path.join(dirpath, name)
            if not _is_macho(p):
                continue
            before = os.path.getsize(p)
            # -x = remove all LOCAL symbols, keep external (global/dynamic) ones,
            # so PyInit_* and the shared wrapper-runtime symbols survive.
            _run(["strip", "-x", p])
            # strip invalidated the signature; re-sign ad-hoc (delocate does the
            # same after its own rewrites).
            _run(["codesign", "--force", "--sign", "-", p])
            saved += before - os.path.getsize(p)
    return saved


def _rewrite_record(root: str) -> None:
    """Recompute sha256/size for every file in the unpacked wheel's RECORD.

    Stripping changed file contents AND sizes, so the existing RECORD hashes are
    stale; ``pip install`` verifies them and would reject the wheel otherwise."""
    dist_info = glob.glob(os.path.join(root, "*.dist-info"))
    if not dist_info:
        raise RuntimeError("no .dist-info in unpacked wheel")
    record_path = os.path.join(dist_info[0], "RECORD")

    rows: list[list[str]] = []
    with open(record_path, newline="") as fh:
        for row in csv.reader(fh):
            if not row:
                continue
            rel = row[0]
            abs = os.path.join(root, rel)
            # The RECORD line for RECORD itself has empty hash/size.
            if os.path.abspath(abs) == os.path.abspath(record_path):
                rows.append([rel, "", ""])
                continue
            with open(abs, "rb") as bf:
                data = bf.read()
            digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest())
            h = "sha256=" + digest.rstrip(b"=").decode("ascii")
            rows.append([rel, h, str(len(data))])

    with open(record_path, "w", newline="") as fh:
        csv.writer(fh).writerows(rows)


def _repack(root: str, out_wheel: str) -> None:
    if os.path.exists(out_wheel):
        os.remove(out_wheel)
    with zipfile.ZipFile(out_wheel, "w", zipfile.ZIP_DEFLATED) as zf:
        for dirpath, _dirs, files in os.walk(root):
            for name in files:
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, root)
                zf.write(full, rel)


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(
            "usage: repair_macos.py <delocate_archs> <dest_dir> <wheel>",
            file=sys.stderr,
        )
        return 2
    archs, dest_dir, wheel = argv[0], argv[1], argv[2]
    os.makedirs(dest_dir, exist_ok=True)

    # 1) Normal delocate repair (bundles + re-signs the vendored dylibs).
    _run(
        [
            "delocate-wheel",
            "--require-archs",
            archs,
            "-w",
            dest_dir,
            "-v",
            wheel,
        ]
    )

    if os.environ.get("CVISTA_STRIP", "1") == "0":
        print("repair_macos: CVISTA_STRIP=0 -> skipping strip pass.", flush=True)
        return 0

    # delocate writes the repaired wheel into dest_dir under the same basename.
    repaired = os.path.join(dest_dir, os.path.basename(wheel))
    if not os.path.exists(repaired):
        # delocate may rename (e.g. platform retag); fall back to newest .whl.
        cands = sorted(
            glob.glob(os.path.join(dest_dir, "*.whl")), key=os.path.getmtime
        )
        if not cands:
            raise RuntimeError(f"delocate produced no wheel in {dest_dir}")
        repaired = cands[-1]

    before = os.path.getsize(repaired)
    work = tempfile.mkdtemp(prefix="cvista-strip-")
    try:
        with zipfile.ZipFile(repaired) as zf:
            zf.extractall(work)
        saved = _strip_and_sign(work)
        _rewrite_record(work)
        _repack(work, repaired)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    after = os.path.getsize(repaired)
    print(
        f"repair_macos: stripped {os.path.basename(repaired)}: "
        f"{before/1e6:.1f} MB -> {after/1e6:.1f} MB "
        f"(binary bytes removed: {saved/1e6:.1f} MB)",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
