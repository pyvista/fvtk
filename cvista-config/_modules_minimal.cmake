# cvista MINIMAL module policy — deny-by-default.
#
# Opposite of _modules.cmake (which does BUILD_ALL_MODULES ON + disable junk).
# Here EVERYTHING is off unless it is in PyVista's measured closure: build only
# the modules PyVista actually imports/dispatches, and let vtk_module_scan pull
# in exactly their dependency closure. ~84 modules instead of ~160.
#
# This exact WANT/NO set was validated against PyVista's full test suite
# (2265 passed / 0 failed, incl. off-screen rendering). The 53 direct C++
# imports + the IO formats PyVista reaches only via reader-factory dispatch
# (IOPLY, IOEnSight, IOCGNSReader, IOChemistry, IOMINC, IOSegY, IOFLUENTCFF,
# IOParallel[XML], IOGeometry) + the two autoinit-critical implementation
# modules (RenderingContextOpenGL2, RenderingGL2PSOpenGL2) that rendering
# silently breaks without.
set(VTK_BUILD_ALL_MODULES OFF CACHE BOOL "")
set(VTK_GROUP_ENABLE_Imaging DONT_WANT CACHE STRING "")
set(VTK_GROUP_ENABLE_MPI DONT_WANT CACHE STRING "")
set(VTK_GROUP_ENABLE_Qt DONT_WANT CACHE STRING "")
set(VTK_GROUP_ENABLE_Rendering DONT_WANT CACHE STRING "")
set(VTK_GROUP_ENABLE_StandAlone DONT_WANT CACHE STRING "")
set(VTK_GROUP_ENABLE_Views DONT_WANT CACHE STRING "")
set(VTK_GROUP_ENABLE_Web DONT_WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_ChartsCore YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_CommonColor WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_CommonComputationalGeometry WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_CommonCore WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_CommonDataModel WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_CommonExecutionModel WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_CommonMath WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_CommonTransforms WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_DomainsChemistry WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersCore WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersExtraction WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersFlowPaths WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersGeneral WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersGeometry WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersHybrid WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersModeling WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersParallelDIY2 WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersParallel WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersPoints WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersPython WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersSources WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersStatistics WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersTexture WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_FiltersVerdict WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_ImagingCore WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_ImagingFourier WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_ImagingGeneral WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_ImagingHybrid WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_ImagingMorphological WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_ImagingSources WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_ImagingStencil WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_InteractionStyle YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_InteractionWidgets YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOCGNSReader WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOChemistry WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOCore WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOEnSight WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOExodus WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOExportGL2PS YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOExport YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOFLUENTCFF WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOGeometry WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOHDF WANT CACHE STRING "")
# core image IO: PNG/JPEG/TIFF/BMP readers+writers for screenshots and textures.
# Enabled explicitly rather than relying on the Web modules' dependency chain to
# pull it in transitively, so image IO can't silently vanish if those deps move.
set(VTK_MODULE_ENABLE_VTK_IOImage WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOImport WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOInfovis WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOLegacy WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOMINC WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOParallel WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOParallelXML WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOPLY WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOSegY WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_IOXML WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_PythonContext2D WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingAnnotation WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingContext2D YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingContextOpenGL2 YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingCore YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingFreeType YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingGL2PSOpenGL2 YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingLabel WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingMatplotlib WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingOpenGL2 YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingUI YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingVolumeOpenGL2 YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_RenderingVolume WANT CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_ViewsContext2D YES CACHE STRING "")
# Web modules: pyvista export_html/export_vtksz -> trame-vtk ->
# vtkmodules.vtkWebCore.vtkWebApplication. WebCore PRIVATE_DEPENDS on
# WebGLExporter (which DEPENDS IOExport, already YES). Explicit module YES
# overrides the VTK_GROUP_ENABLE_Web DONT_WANT above. All transitive deps
# (CommonSystem, ParallelCore, Python, nlohmannjson, vtksys,
# RenderingAnnotation, InteractionWidgets, FiltersCore/General/Geometry,
# IOCore, IOExport, RenderingCore) are already in the enabled closure
# (IOImage is now enabled explicitly above rather than pulled in here).
set(VTK_MODULE_ENABLE_VTK_WebCore YES CACHE STRING "")
set(VTK_MODULE_ENABLE_VTK_WebGLExporter YES CACHE STRING "")

# Explicit NO for heavy/dead modules. With deny-by-default most of these would
# never be scanned, but an explicit NO overrides any stale cached WANT and
# documents intent. xdmf2/IOXdmf2 must be NO: vendored ThirdParty/xdmf2 fails to
# compile on modern libc++ (<strstream> removed) and only backs the rarely-used
# XdmfReader.

# NO speculative "transitive drag" force-NO cuts here. Lesson learned the hard
# way: with BUILD_ALL_MODULES OFF + deny-by-default, a module only builds if an
# ENABLED module DEPENDS on it. So force-NO is both unnecessary (truly-unused
# modules like IOVeraOut/CellGrid never get pulled) AND dangerous — force-NO'ing
# a hidden link-dep silently drops its dependents under WANT (this cost three
# rounds: FiltersHyperTree→rendering stack, RenderingVtkJS→IOExport→IOExportGL2PS).
# Runtime-import traces UNDER-count link deps, so never classify junk by trace
# alone. Let the scanner resolve the closure; only force-NO the heavy modules
# with unavailable EXTERNAL deps (below).

# Newer nightly modules requiring required external libs we don't ship.
# Belt-and-suspenders under deny-by-default (nothing in closure depends on them).
set(VTK_ENABLE_CATALYST OFF CACHE BOOL "")
