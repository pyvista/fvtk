# NOTE: this is included in `_base.cmake`
# Remote modules are not under VTK's development process.
set(VTK_ENABLE_REMOTE_MODULES OFF CACHE BOOL "")
# Enable everything by default, then disable what we don't want.
set(VTK_BUILD_ALL_MODULES ON CACHE BOOL "")

# -----------------------------------------------------------------------------
# Modules requiring software NOT bundled with VTK (same list as Kitware's wheel CI).
# Ref: https://gitlab.kitware.com/vtk/vtk/-/blob/master/.gitlab/ci/configure_wheel.cmake
# -----------------------------------------------------------------------------
set(VTK_MODULE_ENABLE_VTK_CommonArchive NO CACHE STRING "") # libarchive
set(VTK_MODULE_ENABLE_VTK_DomainsMicroscopy NO CACHE STRING "") # openslide
set(VTK_MODULE_ENABLE_VTK_FiltersONNX NO CACHE STRING "") # onnxruntime
set(VTK_MODULE_ENABLE_VTK_FiltersOpenTURNS NO CACHE STRING "") # openturns
set(VTK_MODULE_ENABLE_VTK_FiltersReebGraph NO CACHE STRING "") # boost
set(VTK_MODULE_ENABLE_VTK_GeovisGDAL NO CACHE STRING "") # gdal
set(VTK_MODULE_ENABLE_VTK_IOAlembic NO CACHE STRING "") # alembic
set(VTK_MODULE_ENABLE_VTK_IOADIOS2 NO CACHE STRING "") # adios
set(VTK_MODULE_ENABLE_VTK_IOFFMPEG NO CACHE STRING "") # ffmpeg
set(VTK_MODULE_ENABLE_VTK_IOGDAL NO CACHE STRING "") # gdal
set(VTK_MODULE_ENABLE_VTK_IOIFC NO CACHE STRING "") # IfcOpenShell
set(VTK_MODULE_ENABLE_VTK_IOLAS NO CACHE STRING "") # liblas, boost
set(VTK_MODULE_ENABLE_VTK_IOMySQL NO CACHE STRING "") # mysql
set(VTK_MODULE_ENABLE_VTK_IOODBC NO CACHE STRING "") # odbc
set(VTK_MODULE_ENABLE_VTK_IOOpenVDB NO CACHE STRING "") # OpenVDB
set(VTK_MODULE_ENABLE_VTK_IOPDAL NO CACHE STRING "") # pdal
set(VTK_MODULE_ENABLE_VTK_IOPostgreSQL NO CACHE STRING "") # postgresql
set(VTK_MODULE_ENABLE_VTK_IOUSD NO CACHE STRING "") # OpenUSD
set(VTK_MODULE_ENABLE_VTK_InfovisBoost NO CACHE STRING "") # boost
set(VTK_MODULE_ENABLE_VTK_InfovisBoostGraphAlgorithms NO CACHE STRING "") # boost
set(VTK_MODULE_ENABLE_VTK_RenderingFreeTypeFontConfig NO CACHE STRING "") # fontconfig
set(VTK_MODULE_ENABLE_VTK_RenderingOpenVR NO CACHE STRING "") # openvr
set(VTK_MODULE_ENABLE_VTK_RenderingOpenXR NO CACHE STRING "") # OpenXR
set(VTK_MODULE_ENABLE_VTK_RenderingZSpace NO CACHE STRING "") # zSpace
set(VTK_MODULE_ENABLE_VTK_fides NO CACHE STRING "") # adios
set(VTK_MODULE_ENABLE_VTK_xdmf3 NO CACHE STRING "") # boost
set(VTK_MODULE_ENABLE_VTK_IOOCCT NO CACHE STRING "") # occt
set(VTK_ENABLE_CATALYST OFF CACHE BOOL "") # catalyst
# VTK::conduit (Utilities/Conduit) is third-party EXTERNAL-only (no bundled
# fallback) — it requires a system Conduit install we don't have, so under
# BUILD_ALL_MODULES it fails configure ("Could not find the Conduit external
# dependency"). VTK_ENABLE_CATALYST=OFF does not drop it. Disable it and its
# sole consumer IOCatalystConduit (Catalyst<->Conduit bridge, unused by pyvista).
set(VTK_MODULE_ENABLE_VTK_conduit NO CACHE STRING "") # external conduit, unavailable
set(VTK_MODULE_ENABLE_VTK_IOCatalystConduit NO CACHE STRING "") # needs VTK::conduit

# Newer modules added since this config's source list, each requiring a
# required (PRIVATE_IF_SHARED / third_party_external) external lib we don't
# ship and which the stock pyvista `vtk` wheel also omits. Under
# BUILD_ALL_MODULES they're WANT and fatal-error at find_package time.
set(VTK_MODULE_ENABLE_VTK_IONanoVDB NO CACHE STRING "")            # OpenVDB
set(VTK_MODULE_ENABLE_VTK_RenderingWebGPU NO CACHE STRING "")      # Dawn (webgpu)
set(VTK_MODULE_ENABLE_VTK_RenderingOpenXRRemoting NO CACHE STRING "") # OpenXR
set(VTK_MODULE_ENABLE_VTK_RenderingVR NO CACHE STRING "")          # OpenVR/OpenXR VR

# RenderingRayTracing (ospray) and RenderingAnari are flipped ON in the rt-enabled
# variants (cmake/ospray.cmake, cmake/anari.cmake). Default OFF here.
set(VTK_MODULE_ENABLE_VTK_RenderingRayTracing NO CACHE STRING "") # ospray
set(VTK_MODULE_ENABLE_VTK_RenderingAnari NO CACHE STRING "") # anari

# -----------------------------------------------------------------------------
# Additional drops for a pyvista-focused viewer wheel (codim/pyvista-codim).
# These modules pull in niche formats or subsystems PyVista doesn't use.
# Re-enable individually if you need them.
# -----------------------------------------------------------------------------

# Chemistry (molecular visualisation — mostly unused by pyvista, but
# VTK::DomainsChemistry is a hard DEPENDS of VTK::RenderingRayTracing so it
# cannot be disabled in the OSPRay build. IOChemistry is needed for
# pyvista's GaussianCubeReader / PDBReader tests. DomainsParallelChemistry
# is truly parallel-only.
set(VTK_MODULE_ENABLE_VTK_DomainsParallelChemistry NO CACHE STRING "")

# Niche scientific / simulation IO formats. Kept for pyvista test compat:
# IOEnSight (8 tests), IOChemistry (3 tests), IOXdmf2 (1), IOFLUENTCFF (1).
set(VTK_MODULE_ENABLE_VTK_IOAMR NO CACHE STRING "")              # ParaView AMR
set(VTK_MODULE_ENABLE_VTK_IOAsynchronous NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOCONVERGECFD NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOCesium3DTiles NO CACHE STRING "")    # geospatial tiles
set(VTK_MODULE_ENABLE_VTK_IOCityGML NO CACHE STRING "")          # urban geospatial
set(VTK_MODULE_ENABLE_VTK_IOH5Rage NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOH5part NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOIOSS NO CACHE STRING "")             # Exodus/IOSS (FEA)
set(VTK_MODULE_ENABLE_VTK_IOLSDyna NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOMINC NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOMotionFX NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IONetCDF NO CACHE STRING "")           # climate/atmospheric
set(VTK_MODULE_ENABLE_VTK_IOOMF NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOPIO NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOPLOT3D NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOSegY NO CACHE STRING "")             # seismic
set(VTK_MODULE_ENABLE_VTK_IOSQL NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOTRUCHAS NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOTecplotTable NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOVPIC NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOVeraOut NO CACHE STRING "")

# Video/movie writers (pyvista-stream uses JPEG/NVENC, not these).
set(VTK_MODULE_ENABLE_VTK_IOMovie NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOOggTheora NO CACHE STRING "")

# Views subsystem — pyvista imports vtkContextInteractorStyle from
# vtkViewsContext2D at module load, so ViewsContext2D (and its DEPENDS,
# ViewsCore) must stay enabled. ViewsInfovis is graph-view specific and
# unused by pyvista.
set(VTK_MODULE_ENABLE_VTK_ViewsInfovis NO CACHE STRING "")

# WebCore stays ENABLED: trame-vtk imports vtkmodules.vtkWebCore for its
# VtkLocalView helper; disabling it breaks every pyvista test that calls
# pl.show() (→ export_vtksz → PyVistaLocalView → has_capabilities on None).

# Graph/tree layout infovis (unused by pyvista plotting).
set(VTK_MODULE_ENABLE_VTK_InfovisLayout NO CACHE STRING "")

# Parallel rendering / MPI compositing. We run one EGL context per worker
# process (per-worker VTK_EGL_DEVICE_INDEX round-robin; see
# codimensional/codim#262); no MPI.
set(VTK_MODULE_ENABLE_VTK_RenderingParallel NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingParallelLIC NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingLICOpenGL2 NO CACHE STRING "")  # niche line-integral-convolution vis
# FiltersParallelDIY2 is NOT MPI — it uses the DIY2 block-parallelism library
# bundled with VTK and powers pv.DataSet.partition() / vtkRedistributeDataSetFilter.
# Keep it enabled; the true MPI filters below stay disabled.
set(VTK_MODULE_ENABLE_VTK_FiltersParallelMPI NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersParallelStatistics NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOParallelExodus NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOParallelLSDyna NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOParallelNetCDF NO CACHE STRING "")

set(VTK_GROUP_ENABLE_Qt NO CACHE STRING "")
set(VTK_USE_MPI NO CACHE STRING "")
set(VTK_GROUP_ENABLE_MPI NO CACHE STRING "")

# WebAssembly / Emscripten browser-target modules (newer than the VTK this
# config was first written against). VTK_BUILD_ALL_MODULES requests them, but
# they need a wasm toolchain and have no place in a native CPython wheel —
# without explicit disables, configure fails: "requested or required, but not
# found: SerializationManager;WebAssemblySession;WebAssembly;WebAssemblyAsync".
set(VTK_MODULE_ENABLE_VTK_WebAssembly NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_WebAssemblySession NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_WebAssemblyAsync NO CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_SerializationManager NO CACHE STRING "")
