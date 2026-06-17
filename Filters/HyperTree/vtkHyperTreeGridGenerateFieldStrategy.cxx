// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

// This abstract base class is otherwise header-only: every member is defined
// inline in vtkHyperTreeGridGenerateFieldStrategy.h. It is, however, exported
// (VTKFILTERSHYPERTREE_EXPORT) and referenced by the generated Python wrapper
// and by its concrete subclasses. On ELF platforms (Linux/macOS) the inline
// type-macro/virtual symbols resolve as weak/COMDAT, but MSVC requires a
// defining translation unit in the kit DLL so that dllexport produces import
// entries for `IsTypeOf`, `SafeDownCast`, the virtuals, etc. Compiling this TU
// into VTK::FiltersHyperTree anchors those symbols on every platform, fixing
// the Windows wrapper-link error at the root without affecting behavior.

#include "vtkHyperTreeGridGenerateFieldStrategy.h"

VTK_ABI_NAMESPACE_BEGIN

// No vtkStandardNewMacro: this class is abstract (GetAndFinalizeArray() is
// pure virtual). The out-of-line anchor below gives the class a defining,
// exported translation unit in the kit's shared library.
vtkHyperTreeGridGenerateFieldStrategy::~vtkHyperTreeGridGenerateFieldStrategy() = default;

VTK_ABI_NAMESPACE_END
