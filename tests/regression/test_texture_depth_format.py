"""Regression: vtkTextureObject honors a new depth format on reallocation.

vtkTextureObject::AllocateDepth only applied the requested depth format when no
format was latched yet (``if (!this->Type)`` / ``if (!this->InternalFormat)``).
Once a depth texture had been allocated, ``Type`` and ``InternalFormat`` were
non-zero, so reallocating the same object with a *different* requested depth
format silently respecified storage with the original format. Any caller that
reuses a depth texture across formats (for example to match the depth format of
the framebuffer it blits from) was silently ignored.

The fix records the formats the depth-allocation paths derive themselves and, on
the next depth allocation, drops those derived leftovers before deriving the
newly requested format, while preserving formats a caller set explicitly via
SetDataType / SetInternalFormat.

This test drives the behavior through the public API under software EGL: it
allocates a depth texture with a fixed-point depth format, then reallocates the
same object requesting a float depth format, and asserts the data type follows
the new request. It also checks that an explicitly configured data type survives
a reallocation with a different requested format.

The reallocation path is exercised end to end (a real GL context, a real
glTexImage2D respecify); the assertion is on the object's reported data type,
which is the value the latch bug left stale. The test skips gracefully if a GL
context cannot be created on the host.
"""

import pytest

from cvista.vtkCommonCore import VTK_FLOAT
from cvista.vtkRenderingCore import vtkRenderWindow
import cvista.vtkRenderingOpenGL2  # noqa: F401  (registers the OpenGL factory)
from cvista.vtkRenderingOpenGL2 import vtkTextureObject


def _make_context():
    """Offscreen factory render window with a live GL context, or skip."""
    renWin = vtkRenderWindow()
    renWin.SetOffScreenRendering(1)
    renWin.SetSize(64, 64)
    try:
        # Forces context creation under the harness's surfaceless EGL backend.
        renWin.Render()
    except Exception as exc:  # pragma: no cover - host without a GL backend
        pytest.skip("could not create an OpenGL context: %s" % exc)
    # GetClassName is cheap; a failed context leaves no usable handle.
    if not renWin.SupportsOpenGL():
        pytest.skip("OpenGL context is not usable on this host")
    return renWin


def test_allocate_depth_honors_new_format_on_realloc():
    renWin = _make_context()

    tex = vtkTextureObject()
    tex.SetContext(renWin)

    # First allocation: a fixed-point depth format derives an integer data type.
    assert tex.AllocateDepth(64, 64, vtkTextureObject.Fixed24)
    assert tex.GetVTKDataType() != VTK_FLOAT, (
        "a Fixed24 depth allocation should derive an integer data type"
    )

    # Reallocate the SAME object requesting a float depth format. Before the fix
    # the latched integer data type survived and this assertion failed.
    assert tex.AllocateDepth(64, 64, vtkTextureObject.Float32)
    assert tex.GetVTKDataType() == VTK_FLOAT, (
        "reallocating with Float32 must re-derive a float data type, not keep "
        "the previously latched format"
    )

    # And back again: the reallocation must follow the request in both directions.
    assert tex.AllocateDepth(64, 64, vtkTextureObject.Fixed24)
    assert tex.GetVTKDataType() != VTK_FLOAT, (
        "reallocating back to Fixed24 must re-derive an integer data type"
    )

    tex.ReleaseGraphicsResources(renWin)


def test_explicit_data_type_survives_realloc():
    renWin = _make_context()

    tex = vtkTextureObject()
    tex.SetContext(renWin)

    # A float depth allocation derives a float data type.
    assert tex.AllocateDepth(64, 64, vtkTextureObject.Float32)
    assert tex.GetVTKDataType() == VTK_FLOAT

    # The caller now configures the data type explicitly. Even though its value
    # equals the float default the previous allocation derived, an explicit
    # SetDataType must win over a later allocation's requested format.
    GL_FLOAT = 0x1406  # avoid depending on a GL constant export
    tex.SetDataType(GL_FLOAT)

    # Reallocate requesting a fixed-point format: the explicit data type stays.
    assert tex.AllocateDepth(64, 64, vtkTextureObject.Fixed24)
    assert tex.GetVTKDataType() == VTK_FLOAT, (
        "an explicitly configured data type must survive a reallocation with a "
        "different requested depth format"
    )

    tex.ReleaseGraphicsResources(renWin)
