"""Pixel-exact RENDER scene registry for the fvtk vs stock-VTK regression suite.

Sister to ``tests/bitexact`` but for the rendering pipeline. Every scene here is
built against the *vtkmodules* API only (no pyvista, no ``import vtk``), so the
exact same source drives two backends:

  * stock VTK 9.6.2   -- ``vtkmodules`` resolves to the upstream wheel
  * fvtk (this fork)  -- ``tools/fvtk_shim.py`` redirects ``vtkmodules.* -> fvtk.*``

The only thing that differs between the two render runs is the compiled C++
backend. CRITICAL CONTROL: both sides must render against the *same* EGL/GL
software driver (the host nix Mesa / llvmpipe), so a pixel diff reflects only
fvtk's code, not a different bundled Mesa. The driver records GL_RENDERER /
GL_VERSION on each side; the comparison asserts they match before trusting any
pixel diff.

Determinism rules (so the GL command stream is bit-identical on both sides):
  * Fixed window size, ``SetMultiSamples(0)`` (no MSAA sample-order ambiguity).
  * Fixed deterministic background, fixed camera (explicit position/focal/up,
    no ResetCamera heuristics that could differ).
  * Geometry from deterministic sources (sphere/cone/plane) with fixed
    resolutions; scalars from pure integer/linspace algebra (no sin/cos).
  * No time-, pointer-, or hash-seeded inputs.

Each scene is ``fn() -> (renderer, render_window)`` already wired and sized. The
driver renders it offscreen, reads back the RGBA framebuffer and the Z buffer
via vtkWindowToImageFilter, and dumps both. ``compare.py`` then asserts exact
byte equality of the RGBA buffer (and the Z buffer where captured).
"""
from __future__ import annotations

import numpy as np

# --- vtkmodules imports (resolve to stock vtk OR fvtk depending on the venv) ---
from vtkmodules.vtkCommonCore import vtkFloatArray, vtkPoints, vtkLookupTable
from vtkmodules.vtkCommonDataModel import (
    vtkPolyData,
    vtkCellArray,
    vtkImageData,
    vtkPiecewiseFunction,
)
from vtkmodules.vtkRenderingCore import (
    vtkTexture,
    vtkColorTransferFunction,
    vtkVolume,
    vtkVolumeProperty,
)
from vtkmodules.vtkFiltersTexture import vtkTextureMapToSphere
from vtkmodules.vtkFiltersSources import (
    vtkConeSource,
    vtkSphereSource,
    vtkPlaneSource,
)
from vtkmodules.vtkFiltersCore import vtkGlyph3D, vtkTubeFilter
from vtkmodules.vtkRenderingCore import (
    vtkRenderer,
    vtkRenderWindow,
    vtkPolyDataMapper,
    vtkActor,
)

# Register the OpenGL factory overrides (vtkRenderWindow -> vtkEGLRenderWindow,
# mappers -> vtkOpenGLPolyDataMapper, etc.). Without this the abstract
# RenderingCore classes have no concrete GL backend.
import vtkmodules.vtkRenderingOpenGL2  # noqa: F401

# Register the GPU ray-cast volume backend (vtkSmartVolumeMapper ->
# vtkOpenGLGPUVolumeRayCastMapper). Without this the volume scene has no
# concrete mapper. vtkSmartVolumeMapper itself lives in vtkRenderingVolumeOpenGL2.
from vtkmodules.vtkRenderingVolume import vtkFixedPointVolumeRayCastMapper  # noqa: F401
import vtkmodules.vtkRenderingVolumeOpenGL2  # noqa: F401
from vtkmodules.vtkRenderingVolumeOpenGL2 import vtkSmartVolumeMapper

WIN_W = 256
WIN_H = 256


def _new_window(ren):
    rw = vtkRenderWindow()
    rw.SetOffScreenRendering(1)
    rw.SetMultiSamples(0)
    rw.SetSize(WIN_W, WIN_H)
    rw.AddRenderer(ren)
    return rw


def _fixed_camera(ren, dist=4.0):
    """Deterministic camera -- explicit params, never ResetCamera heuristics."""
    cam = ren.GetActiveCamera()
    cam.SetPosition(dist, 0.6 * dist, 0.8 * dist)
    cam.SetFocalPoint(0.0, 0.0, 0.0)
    cam.SetViewUp(0.0, 1.0, 0.0)
    cam.SetViewAngle(30.0)
    cam.SetClippingRange(0.1, 100.0)


def _renderer(bg=(0.12, 0.18, 0.27)):
    ren = vtkRenderer()
    ren.SetBackground(*bg)
    return ren


# --------------------------------------------------------------------------
# Scenes
# --------------------------------------------------------------------------
def scene_sphere_shaded():
    """A shaded polydata surface (Gouraud-lit sphere)."""
    s = vtkSphereSource()
    s.SetThetaResolution(48)
    s.SetPhiResolution(48)
    s.SetRadius(1.0)
    m = vtkPolyDataMapper()
    m.SetInputConnection(s.GetOutputPort())
    a = vtkActor()
    a.SetMapper(m)
    a.GetProperty().SetColor(0.85, 0.55, 0.30)
    a.GetProperty().SetAmbient(0.18)
    a.GetProperty().SetDiffuse(0.75)
    a.GetProperty().SetSpecular(0.35)
    a.GetProperty().SetSpecularPower(25.0)
    ren = _renderer()
    ren.AddActor(a)
    _fixed_camera(ren, dist=3.2)
    return ren, _new_window(ren)


def scene_cone_shaded():
    """A shaded cone (different primitive count / normals path)."""
    c = vtkConeSource()
    c.SetResolution(40)
    c.SetHeight(1.5)
    c.SetRadius(0.7)
    m = vtkPolyDataMapper()
    m.SetInputConnection(c.GetOutputPort())
    a = vtkActor()
    a.SetMapper(m)
    a.GetProperty().SetColor(0.30, 0.70, 0.85)
    ren = _renderer()
    ren.AddActor(a)
    _fixed_camera(ren, dist=3.0)
    return ren, _new_window(ren)


def scene_scalars_lut():
    """Surface colored by scalars through a lookup table."""
    s = vtkSphereSource()
    s.SetThetaResolution(40)
    s.SetPhiResolution(40)
    s.Update()
    pd = s.GetOutput()
    n = pd.GetNumberOfPoints()
    # Deterministic scalar field: z-coordinate index algebra (no trig).
    sc = vtkFloatArray()
    sc.SetName("scal")
    sc.SetNumberOfTuples(n)
    pts = pd.GetPoints()
    for i in range(n):
        z = pts.GetPoint(i)[2]
        sc.SetValue(i, float(z))
    pd.GetPointData().SetScalars(sc)
    lut = vtkLookupTable()
    lut.SetNumberOfColors(256)
    lut.SetHueRange(0.667, 0.0)
    lut.SetTableRange(-1.0, 1.0)
    lut.Build()
    m = vtkPolyDataMapper()
    m.SetInputData(pd)
    m.SetLookupTable(lut)
    m.SetScalarRange(-1.0, 1.0)
    m.SetScalarModeToUsePointData()
    m.SetColorModeToMapScalars()
    a = vtkActor()
    a.SetMapper(m)
    ren = _renderer()
    ren.AddActor(a)
    _fixed_camera(ren, dist=3.2)
    return ren, _new_window(ren)


def scene_points_glyph():
    """A points/glyph actor: small spheres glyphed onto a point cloud."""
    # Deterministic point cloud on a coarse grid.
    pts = vtkPoints()
    g = 6
    for ix in range(g):
        for iy in range(g):
            for iz in range(g):
                pts.InsertNextPoint(
                    (ix - (g - 1) / 2.0) * 0.4,
                    (iy - (g - 1) / 2.0) * 0.4,
                    (iz - (g - 1) / 2.0) * 0.4,
                )
    cloud = vtkPolyData()
    cloud.SetPoints(pts)
    src = vtkSphereSource()
    src.SetThetaResolution(8)
    src.SetPhiResolution(8)
    src.SetRadius(0.08)
    glyph = vtkGlyph3D()
    glyph.SetInputData(cloud)
    glyph.SetSourceConnection(src.GetOutputPort())
    glyph.SetScaleModeToDataScalingOff()
    m = vtkPolyDataMapper()
    m.SetInputConnection(glyph.GetOutputPort())
    a = vtkActor()
    a.SetMapper(m)
    a.GetProperty().SetColor(0.9, 0.8, 0.2)
    ren = _renderer()
    ren.AddActor(a)
    _fixed_camera(ren, dist=4.0)
    return ren, _new_window(ren)


def scene_tube_lines():
    """A thin-line / tube actor along a deterministic polyline."""
    pts = vtkPoints()
    npts = 60
    for i in range(npts):
        t = i / (npts - 1.0)
        # Deterministic helix-free zigzag (no trig): piecewise-linear curve.
        x = 2.0 * t - 1.0
        y = 0.5 * (((i % 8) / 7.0) - 0.5)
        z = 0.3 * (((i % 5) / 4.0) - 0.5)
        pts.InsertNextPoint(x, y, z)
    lines = vtkCellArray()
    lines.InsertNextCell(npts)
    for i in range(npts):
        lines.InsertCellPoint(i)
    poly = vtkPolyData()
    poly.SetPoints(pts)
    poly.SetLines(lines)
    tube = vtkTubeFilter()
    tube.SetInputData(poly)
    tube.SetRadius(0.04)
    tube.SetNumberOfSides(12)
    m = vtkPolyDataMapper()
    m.SetInputConnection(tube.GetOutputPort())
    a = vtkActor()
    a.SetMapper(m)
    a.GetProperty().SetColor(0.2, 0.9, 0.5)
    ren = _renderer()
    ren.AddActor(a)
    _fixed_camera(ren, dist=2.6)
    return ren, _new_window(ren)


def scene_edges():
    """A simple actor with edges drawn (wireframe-on-surface)."""
    s = vtkSphereSource()
    s.SetThetaResolution(16)
    s.SetPhiResolution(16)
    m = vtkPolyDataMapper()
    m.SetInputConnection(s.GetOutputPort())
    a = vtkActor()
    a.SetMapper(m)
    a.GetProperty().SetColor(0.7, 0.3, 0.3)
    a.GetProperty().EdgeVisibilityOn()
    a.GetProperty().SetEdgeColor(0.95, 0.95, 0.95)
    a.GetProperty().SetLineWidth(1.0)
    ren = _renderer()
    ren.AddActor(a)
    _fixed_camera(ren, dist=3.2)
    return ren, _new_window(ren)


def scene_textured():
    """A textured sphere -- exercises the mapper's texture binding/uniform path
    (HaveTextures / GetTextures), which the other scenes do not touch."""
    s = vtkSphereSource()
    s.SetThetaResolution(40)
    s.SetPhiResolution(40)
    tmap = vtkTextureMapToSphere()
    tmap.SetInputConnection(s.GetOutputPort())
    tmap.PreventSeamOn()
    # Deterministic 16x16 RGB checker texture (pure integer algebra, no trig).
    img = vtkImageData()
    img.SetDimensions(16, 16, 1)
    img.AllocateScalars(3, 3)  # VTK_UNSIGNED_CHAR=3, 3 components
    sc = img.GetPointData().GetScalars()
    idx = 0
    for j in range(16):
        for i in range(16):
            on = ((i // 2) + (j // 2)) % 2
            sc.SetTuple3(idx, 230 if on else 40, 120, 60 if on else 200)
            idx += 1
    tex = vtkTexture()
    tex.SetInputData(img)
    tex.InterpolateOff()
    m = vtkPolyDataMapper()
    m.SetInputConnection(tmap.GetOutputPort())
    a = vtkActor()
    a.SetMapper(m)
    a.SetTexture(tex)
    ren = _renderer()
    ren.AddActor(a)
    _fixed_camera(ren, dist=3.2)
    return ren, _new_window(ren)


def _checker_texture(c0, c1):
    """Deterministic 16x16 RGB checker texture (pure integer algebra, no trig)."""
    img = vtkImageData()
    img.SetDimensions(16, 16, 1)
    img.AllocateScalars(3, 3)  # VTK_UNSIGNED_CHAR=3, 3 components
    sc = img.GetPointData().GetScalars()
    idx = 0
    for j in range(16):
        for i in range(16):
            on = ((i // 2) + (j // 2)) % 2
            r, g, b = c1 if on else c0
            sc.SetTuple3(idx, r, g, b)
            idx += 1
    tex = vtkTexture()
    tex.SetInputData(img)
    tex.InterpolateOff()
    return tex


def scene_multi_textured():
    """Several actors of mixed kind (two textured spheres + one scalar-colored
    sphere) drawn into one renderer. Multiple textured actors per frame amplify
    the per-actor, per-frame texture-binding path (GetTextures -> texinfo vector
    + uniform-name lookups) far more than a single textured sphere does, so a
    regression in that path (e.g. a stale/dropped texture set) shows up here."""
    ren = _renderer()
    # Two textured spheres at offset positions, distinct deterministic checkers.
    for k, (cx, c0, c1) in enumerate(
        [
            (-1.1, (40, 120, 200), (230, 120, 60)),
            (1.1, (30, 200, 90), (210, 60, 160)),
        ]
    ):
        s = vtkSphereSource()
        s.SetThetaResolution(32)
        s.SetPhiResolution(32)
        s.SetRadius(0.8)
        s.SetCenter(cx, 0.0, 0.0)
        tmap = vtkTextureMapToSphere()
        tmap.SetInputConnection(s.GetOutputPort())
        tmap.PreventSeamOn()
        m = vtkPolyDataMapper()
        m.SetInputConnection(tmap.GetOutputPort())
        a = vtkActor()
        a.SetMapper(m)
        a.SetTexture(_checker_texture(c0, c1))
        ren.AddActor(a)
    # One scalar-colored sphere through a LUT (the ColorTextureMap path -> adds
    # the implicit "colortexture" entry to the texture set on that actor).
    s = vtkSphereSource()
    s.SetThetaResolution(32)
    s.SetPhiResolution(32)
    s.SetRadius(0.7)
    s.SetCenter(0.0, 1.1, 0.0)
    s.Update()
    pd = s.GetOutput()
    n = pd.GetNumberOfPoints()
    sc = vtkFloatArray()
    sc.SetName("scal")
    sc.SetNumberOfTuples(n)
    pts = pd.GetPoints()
    for i in range(n):
        sc.SetValue(i, float(pts.GetPoint(i)[1]))
    pd.GetPointData().SetScalars(sc)
    lut = vtkLookupTable()
    lut.SetNumberOfColors(256)
    lut.SetHueRange(0.0, 0.667)
    lut.SetTableRange(0.0, 2.0)
    lut.Build()
    m = vtkPolyDataMapper()
    m.SetInputData(pd)
    m.SetLookupTable(lut)
    m.SetScalarRange(0.0, 2.0)
    m.SetScalarModeToUsePointData()
    m.SetColorModeToMapScalars()
    a = vtkActor()
    a.SetMapper(m)
    ren.AddActor(a)
    _fixed_camera(ren, dist=4.6)
    return ren, _new_window(ren)


def _translucent_sphere(cx, cy, cz, color, opacity, res=32):
    """A single semi-transparent sphere actor at a fixed center."""
    s = vtkSphereSource()
    s.SetThetaResolution(res)
    s.SetPhiResolution(res)
    s.SetRadius(1.0)
    s.SetCenter(cx, cy, cz)
    m = vtkPolyDataMapper()
    m.SetInputConnection(s.GetOutputPort())
    a = vtkActor()
    a.SetMapper(m)
    a.GetProperty().SetColor(*color)
    a.GetProperty().SetOpacity(opacity)
    return a


def scene_depth_peeling():
    """Multiple overlapping semi-transparent spheres -> order-independent
    transparency via depth peeling.

    This is the transparency-pass analogue of the opaque scenes: with
    ``UseDepthPeeling`` on and several mutually overlapping translucent actors,
    the renderer drives ``vtkDualDepthPeelingPass`` (the default when the GL
    driver supports it) / ``vtkDepthPeelingPass`` through several peel layers
    plus the per-layer FBO/texture ping-pong, occlusion queries and blend
    compositing. The pixel output depends EXACTLY on the peel-layer count and
    per-layer blending, so this scene is the bit-exact gate for any CPU-side
    optimization of those passes.

    Determinism: fixed camera, fixed peel cap + zero occlusion ratio (so the
    layer count is a pure function of the geometry, not a runtime ratio), no
    MSAA. The three spheres overlap pairwise along the view direction, so the
    peeler must resolve several translucent layers per pixel.
    """
    ren = _renderer()
    # Three overlapping translucent spheres of distinct color/opacity. The
    # offsets are small relative to the unit radius so they interpenetrate,
    # forcing multiple peel layers along most camera rays.
    ren.AddActor(_translucent_sphere(-0.5, 0.0, 0.0, (0.90, 0.20, 0.20), 0.45))
    ren.AddActor(_translucent_sphere(0.5, 0.0, 0.0, (0.20, 0.85, 0.30), 0.45))
    ren.AddActor(_translucent_sphere(0.0, 0.0, 0.6, (0.25, 0.40, 0.95), 0.45))

    ren.SetUseDepthPeeling(1)
    ren.SetMaximumNumberOfPeels(8)
    ren.SetOcclusionRatio(0.0)  # peel until layers exhausted (deterministic)

    _fixed_camera(ren, dist=3.4)
    return ren, _new_window(ren)


def scene_depth_peeling_dense():
    """A denser translucent stack: a 3x3 grid of overlapping translucent
    spheres seen edge-on, exercising a higher peel-layer count and more
    intermediate-peel blend passes than ``scene_depth_peeling``."""
    ren = _renderer()
    colors = [
        (0.90, 0.30, 0.30),
        (0.30, 0.80, 0.40),
        (0.35, 0.45, 0.95),
    ]
    k = 0
    for ix in range(3):
        for iy in range(3):
            cx = (ix - 1) * 0.7
            cy = (iy - 1) * 0.7
            cz = ((ix + iy) % 3 - 1) * 0.5
            ren.AddActor(
                _translucent_sphere(cx, cy, cz, colors[k % 3], 0.40, res=24)
            )
            k += 1

    ren.SetUseDepthPeeling(1)
    ren.SetMaximumNumberOfPeels(12)
    ren.SetOcclusionRatio(0.0)

    _fixed_camera(ren, dist=4.2)
    return ren, _new_window(ren)


def scene_volume_raycast():
    """GPU ray-cast volume rendering of a deterministic 64^3 scalar field.

    Exercises the heavy PyVista add_volume path: vtkSmartVolumeMapper (-> GPU
    ray cast) + a color transfer function + an opacity (piecewise) function +
    vtkVolumeProperty. The scalar field, transfer functions, sample distance,
    and camera are all fixed/deterministic so the GL command stream is identical
    on both backends. This is the STEP-1 de-risk scene: ray casting uses 3D
    textures + fragment-shader compositing (more FP / driver-dependent than
    surface rendering), so we render it through the SAME host Mesa on both sides
    and diff pixel-for-pixel.
    """
    dim = 64
    img = vtkImageData()
    img.SetDimensions(dim, dim, dim)
    img.SetSpacing(1.0, 1.0, 1.0)
    img.SetOrigin(0.0, 0.0, 0.0)
    img.AllocateScalars(11, 1)  # VTK_FLOAT=11, 1 component
    sc = img.GetPointData().GetScalars()
    sc.SetName("vol")
    # Deterministic field: a radial "blob" via pure integer/float algebra
    # (squared distance from center, no trig), scaled into [0, 255].
    c = (dim - 1) / 2.0
    rmax2 = 3.0 * c * c
    idx = 0
    for k in range(dim):
        dk = k - c
        for j in range(dim):
            dj = j - c
            for i in range(dim):
                di = i - c
                d2 = di * di + dj * dj + dk * dk
                # Falls off from 255 at center to 0 at the corner; deterministic.
                v = 255.0 * (1.0 - d2 / rmax2)
                if v < 0.0:
                    v = 0.0
                sc.SetValue(idx, v)
                idx += 1

    color = vtkColorTransferFunction()
    color.AddRGBPoint(0.0, 0.0, 0.0, 0.0)
    color.AddRGBPoint(64.0, 0.2, 0.1, 0.6)
    color.AddRGBPoint(128.0, 0.1, 0.7, 0.3)
    color.AddRGBPoint(192.0, 0.9, 0.6, 0.1)
    color.AddRGBPoint(255.0, 1.0, 1.0, 0.9)

    opacity = vtkPiecewiseFunction()
    opacity.AddPoint(0.0, 0.0)
    opacity.AddPoint(64.0, 0.02)
    opacity.AddPoint(128.0, 0.08)
    opacity.AddPoint(255.0, 0.25)

    prop = vtkVolumeProperty()
    prop.SetColor(color)
    prop.SetScalarOpacity(opacity)
    prop.SetInterpolationTypeToLinear()
    prop.ShadeOff()

    mapper = vtkSmartVolumeMapper()
    mapper.SetInputData(img)
    # Force the GPU ray-cast path and a FIXED sample distance so the compositing
    # step count is deterministic (no auto-sample-distance heuristics).
    mapper.SetRequestedRenderModeToGPU()
    mapper.SetSampleDistance(0.5)
    mapper.AutoAdjustSampleDistancesOff()
    mapper.SetBlendModeToComposite()

    vol = vtkVolume()
    vol.SetMapper(mapper)
    vol.SetProperty(prop)

    ren = _renderer()
    ren.AddVolume(vol)
    # Camera placed explicitly relative to the volume center (dim/2 in each axis).
    cam = ren.GetActiveCamera()
    half = dim / 2.0
    cam.SetFocalPoint(half, half, half)
    cam.SetPosition(half + 3.0 * dim, half + 0.6 * dim, half + 0.8 * dim)
    cam.SetViewUp(0.0, 1.0, 0.0)
    cam.SetViewAngle(30.0)
    cam.SetClippingRange(0.1, 1000.0)
    return ren, _new_window(ren)


SCENES = {
    "sphere_shaded": scene_sphere_shaded,
    "textured": scene_textured,
    "multi_textured": scene_multi_textured,
    "volume_raycast": scene_volume_raycast,
    "cone_shaded": scene_cone_shaded,
    "scalars_lut": scene_scalars_lut,
    "points_glyph": scene_points_glyph,
    "tube_lines": scene_tube_lines,
    "edges": scene_edges,
    "depth_peeling": scene_depth_peeling,
    "depth_peeling_dense": scene_depth_peeling_dense,
}


def iter_scenes():
    for name in SCENES:
        yield name
