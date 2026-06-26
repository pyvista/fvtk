# cvista source-unity exclusion lists (consumed by the CVISTA_SOURCE_UNITY hook in
# CMake/vtkModule.cmake).
#
# CVISTA_SOURCE_UNITY_EXCLUDE — whole MODULE library names (e.g. vtkFiltersCore)
# excluded from UNITY_BUILD entirely. Reserved for a module so unity-hostile that
# per-file skips aren't enough; currently empty (per-file skips cover every known
# breaker). vtkCommonCore is excluded unconditionally in the hook (array-TU split,
# PR #27) and ThirdParty modules are excluded unconditionally too — neither is
# listed here.
#
# CVISTA_SOURCE_UNITY_SKIP_FILES — individual source .cxx BASENAMES that are
# unity-batch breakers and get pulled into their own standalone TU while the rest
# of their module still batches. (Generated *Instantiate*.cxx are skipped
# automatically by a name pattern in the hook and are NOT listed here.)
#
# These are the hand-written files whose file-local anonymous-namespace globals,
# static consts, typedefs or `using`-brought constants (e.g. VTK_DIVERGED) collide
# by NAME with an identically-named entity in another batched TU of the same
# module. Object code for the skipped file is unchanged; it just compiles alone.
# Empirically derived from a full GCC-14 source-unity build (ninja -k 0) — see the
# PR for the build log. Adding a file here is bit-exact-neutral (it only changes
# which TU a .cxx lands in, never the emitted code).
#
# Both MUST be CACHE INTERNAL so the values survive from this `-C` initial-cache
# file into the project/function scope where the hook reads them (a plain set()
# does not persist past the init-cache pass — same reason the NOCOMPILE/NOWRAP
# lists close with CACHE INTERNAL).

set(CVISTA_SOURCE_UNITY_EXCLUDE
  ""
  CACHE INTERNAL "cvista: modules excluded from source UNITY_BUILD batching")

set(CVISTA_SOURCE_UNITY_SKIP_FILES
  # --- Common/DataModel cell classes: file-local `edges`/`faces`/`MidPoints`/
  #     `*CASES_t` tables + VTK_DIVERGED in anonymous namespace, name-collide ---
  vtkTriangle.cxx
  vtkQuad.cxx
  vtkPixel.cxx
  vtkHexahedron.cxx
  vtkWedge.cxx
  vtkQuadraticWedge.cxx
  vtkQuadraticHexahedron.cxx
  vtkQuadraticPyramid.cxx
  vtkQuadraticQuad.cxx
  vtkQuadraticTetra.cxx
  vtkTriQuadraticPyramid.cxx
  vtkBiQuadraticQuadraticWedge.cxx
  vtkQuadraticLinearWedge.cxx
  # HyperTreeGrid Moore/VonNeumann super-cursors each #include the same
  # *SuperCursorData.inl global lookup tables (file-scope const arrays) -> the
  # tables redefine when two of these land in one batch. Skip all five.
  vtkHyperTreeGridNonOrientedMooreSuperCursor.cxx
  vtkHyperTreeGridNonOrientedMooreSuperCursorLight.cxx
  vtkHyperTreeGridNonOrientedUnlimitedMooreSuperCursor.cxx
  vtkHyperTreeGridNonOrientedVonNeumannSuperCursor.cxx
  vtkHyperTreeGridNonOrientedVonNeumannSuperCursorLight.cxx
  # --- Filters/* anonymous-namespace functor / table collisions ---
  vtkAppendPolyData.cxx
  vtkResampleWithDataSet.cxx
  vtkStaticCleanUnstructuredGrid.cxx
  vtkStaticPointLocator2D.cxx
  vtkGenerateGlobalIds.cxx
  vtkCookieCutter.cxx
  vtkFitImplicitFunction.cxx
  vtkPointInterpolator2D.cxx
  vtkPolyDataPlaneCutter.cxx
  vtkParametricSuperToroid.cxx
  vtkSurfaceNets3D.cxx
  vtkSPHInterpolator.cxx
  # --- Filters/HyperTree ---
  vtkHyperTreeGridPlaneCutter.cxx
  # --- Filters/CellGrid DG cell classes ---
  vtkDGQuad.cxx
  vtkDGTet.cxx
  vtkDGTri.cxx
  vtkDGVert.cxx
  vtkDGWdg.cxx
  vtkDGPyr.cxx
  # --- Rendering/OpenGL2 mapper anonymous-namespace helpers ---
  vtkOpenGLPolyDataMapper.cxx
  vtkOpenGLLowMemoryPolyDataMapper.cxx
  # vtkXOpenGLRenderWindow pulls the X11 headers (Xlib/Xutil/Xatom), which
  # #define a swarm of short tokens (MWM_*, plus X11's own macros) that leak into
  # a later batched file's vtkGenericDataArray.h and corrupt the DoComputeRange
  # template macros ("expected identifier before numeric constant"). Skip it.
  vtkXOpenGLRenderWindow.cxx
  vtkXWebGPURenderWindow.cxx
  # --- Interaction/Widgets ---
  vtkTexturedButtonRepresentation2D.cxx
  # --- Charts/Core ---
  vtkPlotStacked.cxx
  # --- IO/* readers/writers with file-local statics ---
  vtkEnSightGoldBinaryReader.cxx
  vtkMINCImageWriter.cxx
  vtkNIFTIImageWriter.cxx
  vtkPLYWriter.cxx
  # Vendored third-party miniply parser (its own `namespace miniply`, plus
  # file-scope helpers); compile standalone so it never shares a batch with the
  # IOPLY reader/writer TUs.
  miniply.cxx
  CACHE INTERNAL "cvista: source .cxx basenames pulled out of UNITY_BUILD batching")
