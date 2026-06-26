"""Standalone driver: render every scene offscreen via EGL, read back the RGBA
framebuffer (and Z buffer), and dump one .npz per scene plus a manifest.json.

Invoked once under the stock-VTK venv and once under the cvista venv (the
``vtkmodules`` import resolves to whichever backend that venv provides). The
pytest harness / CI job then compare the two output dirs pixel-for-pixel.

Usage:  python run_render.py <output_dir> [scene_filter]
  scene_filter: optional substring; only run scenes whose name contains it.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render_ops  # noqa: E402

from vtkmodules.vtkCommonCore import vtkUnsignedCharArray  # noqa: E402
from vtkmodules.util.numpy_support import vtk_to_numpy  # noqa: E402


def _rgba(render_window):
    """Read back the full RGBA framebuffer as a (H, W, 4) uint8 array."""
    w, h = render_window.GetSize()
    out = vtkUnsignedCharArray()
    render_window.GetRGBACharPixelData(0, 0, w - 1, h - 1, 0, out)  # front=0
    np_arr = vtk_to_numpy(out).reshape(h, w, 4)
    return np.ascontiguousarray(np_arr)


def _zbuf(render_window):
    """Read back the depth buffer as a (H, W) float32 array (may be unavailable)."""
    from vtkmodules.vtkCommonCore import vtkFloatArray as _vfa

    w, h = render_window.GetSize()
    try:
        out = _vfa()
        render_window.GetZbufferData(0, 0, w - 1, h - 1, out)
        np_arr = vtk_to_numpy(out).reshape(h, w)
        return np.ascontiguousarray(np_arr.astype(np.float32))
    except Exception:
        return None


def buf_sha(arrays):
    h = hashlib.sha256()
    for name in sorted(arrays):
        a = np.ascontiguousarray(arrays[name])
        h.update(name.encode())
        h.update(str(a.dtype).encode())
        h.update(str(a.shape).encode())
        h.update(a.tobytes())
    return h.hexdigest()


def gl_caps(render_window):
    caps = {}
    try:
        report = render_window.ReportCapabilities()
        for line in report.splitlines():
            ls = line.strip()
            if ls.startswith("OpenGL renderer string:"):
                caps["gl_renderer"] = ls.split(":", 1)[1].strip()
            elif ls.startswith("OpenGL version string:"):
                caps["gl_version"] = ls.split(":", 1)[1].strip()
            elif ls.startswith("OpenGL vendor string:"):
                caps["gl_vendor"] = ls.split(":", 1)[1].strip()
    except Exception as e:  # noqa: BLE001
        caps["error"] = repr(e)
    return caps


def main():
    out = sys.argv[1]
    scene_filter = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(out, exist_ok=True)

    from vtkmodules.vtkCommonCore import vtkVersion
    import vtkmodules

    manifest = {"cases": {}}
    first_caps = None
    n_ok = n_err = 0

    for name in render_ops.iter_scenes():
        if scene_filter and scene_filter not in name:
            continue
        try:
            ren, rw = render_ops.SCENES[name]()
            rw.Render()
            if first_caps is None:
                first_caps = gl_caps(rw)
            arrays = {"rgba": _rgba(rw)}
            z = _zbuf(rw)
            if z is not None:
                arrays["z"] = z
            window_class = rw.GetClassName()
            np.savez(os.path.join(out, name + ".npz"), **arrays)
            manifest["cases"][name] = {
                "scene": name,
                "window_class": window_class,
                "size": list(rw.GetSize()),
                "n_arrays": len(arrays),
                "has_z": z is not None,
                "sha256": buf_sha(arrays),
                "rgba_shape": list(arrays["rgba"].shape),
                "rgba_nonbg_pixels": int(
                    np.count_nonzero(np.any(arrays["rgba"][..., :3] != arrays["rgba"][0, 0, :3], axis=-1))
                ),
            }
            # Free the GL context promptly.
            rw.Finalize()
            n_ok += 1
        except Exception as e:  # noqa: BLE001
            manifest["cases"][name] = {"scene": name, "error": repr(e)}
            n_err += 1
            print(f"ERROR {name}: {e!r}", file=sys.stderr)

    provenance = {
        "numpy": np.__version__,
        "vtk_version": vtkVersion.GetVTKVersion(),
        "vtkmodules_file": getattr(vtkmodules, "__file__", "?"),
        "gl": first_caps or {},
    }
    manifest["provenance"] = provenance

    with open(os.path.join(out, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)

    print(
        f"[run_render] backend vtkmodules={provenance['vtkmodules_file']} "
        f"vtk={provenance['vtk_version']} numpy={provenance['numpy']}"
    )
    print(f"[run_render] GL = {provenance['gl']}")
    print(f"[run_render] wrote {n_ok} scenes, {n_err} errors -> {out}")
    return 1 if n_err else 0


if __name__ == "__main__":
    sys.exit(main())
