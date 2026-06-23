# fvtk: vendor mimalloc (microsoft/mimalloc, pinned v2.3.2) as an in-tree SHARED
# library (libvtkmimalloc.so / .dylib / vtkmimalloc.dll). It is the single,
# process-global allocator instance that backs fvtk's per-kit global operator
# new/delete override (Common/Core/vtkFVTKAllocator.cxx, injected HIDDEN into
# every fvtk shared library by the top-level CMakeLists.txt); see that file and
# ThirdParty/mimalloc/vtkmimalloc/CMakeLists.txt for the single-instance /
# no-interposition design.
#
# Shared (NOT static-per-kit) is the correctness requirement: every kit's override
# must forward to ONE mimalloc instance so a block allocated through one kit and
# freed through another resolves against the SAME segment map. A static mimalloc
# linked into each kit would give each kit its own allocator metadata and corrupt
# the heap on a cross-kit/cross-DLL free.
#
# The module follows BUILD_SHARED_LIBS (ON for the wheel build), so it builds as
# a shared library on all three shipped platforms (Linux, macOS, Windows). It is
# enabled (VTK_MODULE_ENABLE_VTK_mimalloc=YES) whenever the FVTK_MIMALLOC lever is
# ON (the default), on all platforms. The one shared mimalloc lib is a normal
# linked dependency of every fvtk kit, so auditwheel (Linux) / delocate (macOS) /
# delvewheel (Windows) bundle it into the wheel like any other dependency.
vtk_module_third_party_internal(
    LICENSE_FILES
      "vtkmimalloc/LICENSE"
    SPDX_LICENSE_IDENTIFIER
      "MIT"
    SPDX_COPYRIGHT_TEXT
      "Copyright (c) 2018-2025 Microsoft Corporation, Daan Leijen"
    SPDX_DOWNLOAD_LOCATION
      "git+https://github.com/microsoft/mimalloc.git@v2.3.2"
    VERSION
      "2.3.2"
    STANDARD_INCLUDE_DIRS)
