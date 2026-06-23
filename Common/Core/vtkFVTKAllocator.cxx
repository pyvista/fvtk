// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
//
// fvtk (pyvista/fvtk) addition -- NOT part of upstream VTK.
//
// Global C++ operator new/delete override that routes fvtk's own heap
// allocations to the single, process-global, vendored mimalloc instance
// (ThirdParty/mimalloc -> the shared libvtkmimalloc.so/.dylib/vtkmimalloc.dll).
// This is the ONLY allocator hook in fvtk; there is no LD_PRELOAD, no mimalloc
// malloc/free interposition, and no mimalloc redirect/preload mechanism.
//
// PER-KIT, LIBRARY-WIDE: this TU is compiled (HIDDEN) into EVERY fvtk shared
// library -- every kit (vtkCommon, vtkFilters, vtkRendering, ...), every
// standalone module DLL, and every Python wrapper module -- by the post-build
// injection in the top-level CMakeLists.txt. Each library therefore gets its OWN
// hidden operator new/delete that binds within that library and forwards to the
// SAME shared mimalloc instance via mi_malloc_aligned/mi_free. Because all the
// overrides call into ONE mimalloc (one segment/heap metadata map), a block
// allocated through one kit and freed through another is consistent -- no
// cross-kit / cross-DLL heap corruption. (A static-per-kit mimalloc would give
// each kit its OWN metadata and corrupt the heap on such a free; that is exactly
// what this shared-instance design avoids, and why it is correct on Windows too,
// where every DLL otherwise has its own CRT heap.)
//
// WHY THIS IS SAFE (no host interposition / "just works" on pip install):
//   * fvtk is built with CMAKE_CXX_VISIBILITY_PRESET=hidden +
//     CMAKE_VISIBILITY_INLINES_HIDDEN and -fno-semantic-interposition. These
//     operator new/delete definitions carry NO export macro, so on ELF/Mach-O
//     they are emitted as HIDDEN symbols: they satisfy each fvtk library's own
//     intra-.so operator new/delete references but are NOT placed in the dynamic
//     symbol table, so they cannot interpose the host CPython allocator,
//     libstdc++, or any other extension module. On MSVC, user replacement of the
//     global operator new/delete is a documented, supported facility and is
//     scoped to the DLL that defines it (PE/COFF has no cross-DLL symbol
//     interposition), so the host CPython CRT is likewise untouched. The
//     cross-platform CI guard (ci/check-no-alloc-exports.sh) asserts on the
//     built artifacts -- nm on Linux/macOS, dumpbin/llvm-nm on Windows -- that no
//     fvtk library exports malloc/free/operator new/delete, and fails the build
//     otherwise.
//   * mimalloc itself is compiled WITHOUT MI_MALLOC_OVERRIDE, so the shared
//     mimalloc library exports ONLY the mi_* C API that this TU calls -- never
//     malloc/free/operator new.
//
// WHY THIS IS BYTE-EXACT: an allocator only changes the ADDRESSES returned by
// new/malloc; it never changes any value, count, ordering, or layout that fvtk
// computes. The bitexact (maxULP=0), renderexact, and pyvista parity gates are
// the proof.

#include <cstddef>
#include <new>

#include <mimalloc.h>

// All of these are intentionally NOT marked with any VTK*_EXPORT macro: under
// the build's hidden default visibility (ELF/Mach-O) they resolve fvtk-internal
// references only and are never exported; on MSVC they replace the global
// operator new/delete for the defining DLL only. mimalloc's mi_malloc_aligned
// honors C++'s fundamental alignment guarantee; we pass alignof(std::max_align_t)
// for the unaligned overloads (matching the platform malloc contract) and the
// requested alignment for the aligned (C++17) overloads.

namespace
{
constexpr std::size_t kFvtkMaxAlign = alignof(std::max_align_t);

inline void* fvtk_mi_alloc(std::size_t size, std::size_t align)
{
  // mimalloc requires a non-zero allocation to always return a unique pointer;
  // mi_malloc_aligned(0, ...) already returns a valid unique pointer, matching
  // the operator new(0) requirement.
  return mi_malloc_aligned(size, align);
}
} // namespace

// ---- throwing operator new ----------------------------------------------------
void* operator new(std::size_t size)
{
  void* p = fvtk_mi_alloc(size, kFvtkMaxAlign);
  if (!p)
  {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new[](std::size_t size)
{
  void* p = fvtk_mi_alloc(size, kFvtkMaxAlign);
  if (!p)
  {
    throw std::bad_alloc();
  }
  return p;
}

// ---- nothrow operator new -----------------------------------------------------
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  return fvtk_mi_alloc(size, kFvtkMaxAlign);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  return fvtk_mi_alloc(size, kFvtkMaxAlign);
}

// ---- aligned operator new (C++17) ---------------------------------------------
void* operator new(std::size_t size, std::align_val_t al)
{
  void* p = fvtk_mi_alloc(size, static_cast<std::size_t>(al));
  if (!p)
  {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new[](std::size_t size, std::align_val_t al)
{
  void* p = fvtk_mi_alloc(size, static_cast<std::size_t>(al));
  if (!p)
  {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept
{
  return fvtk_mi_alloc(size, static_cast<std::size_t>(al));
}

void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept
{
  return fvtk_mi_alloc(size, static_cast<std::size_t>(al));
}

// ---- operator delete ----------------------------------------------------------
// mi_free safely handles a null pointer and pointers from any mi_malloc_aligned
// alignment, so every delete variant routes through it. Sized/aligned deletes
// pass the same pointer; mimalloc tracks the block internally.
void operator delete(void* p) noexcept
{
  mi_free(p);
}

void operator delete[](void* p) noexcept
{
  mi_free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept
{
  mi_free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept
{
  mi_free(p);
}

// sized delete (C++14)
void operator delete(void* p, std::size_t) noexcept
{
  mi_free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
  mi_free(p);
}

// aligned delete (C++17)
void operator delete(void* p, std::align_val_t) noexcept
{
  mi_free(p);
}

void operator delete[](void* p, std::align_val_t) noexcept
{
  mi_free(p);
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
  mi_free(p);
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
  mi_free(p);
}

void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
  mi_free(p);
}

void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
  mi_free(p);
}
