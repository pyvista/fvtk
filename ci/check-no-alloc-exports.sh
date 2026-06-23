#!/usr/bin/env bash
# fvtk: cross-platform "no host interposition" safety gate.
#
# Asserts that NONE of the fvtk shared libraries export an allocator symbol
# (malloc/free/calloc/realloc/operator new/operator delete) in their PUBLIC
# (dynamic / exported) symbol table. fvtk routes its own C++ operator new/delete
# to a single shared mimalloc instance (Common/Core/vtkFVTKAllocator.cxx ->
# libvtkmimalloc), but that override MUST stay fvtk-internal: on ELF/Mach-O it is
# built with hidden visibility + -fno-semantic-interposition so it binds within
# each fvtk library and is NEVER placed in the dynamic symbol table; on Windows
# the global operator new/delete replacement is scoped to the defining DLL (PE/
# COFF has no cross-DLL interposition). If any of malloc/free/calloc/realloc/
# operator new/operator delete ever appears as an EXPORTED symbol, fvtk would
# interpose the host CPython allocator (and every other loaded extension's
# allocator) -- exactly the LD_PRELOAD-style behavior we forbid. This guard fails
# the build in that case.
#
# The shared mimalloc lib (libvtkmimalloc*) legitimately EXPORTS the mi_* C API
# (mi_malloc/mi_free/...): that is its public interface, consumed by every kit's
# override across the .so/.dll boundary. mi_* is NOT the CRT malloc/operator new,
# so it does not interpose the host -- the guard allows mi_* and forbids only the
# CRT/operator-new family.
#
# Platforms:
#   * Linux  : nm -D --defined-only            (ELF .dynsym exports)
#   * macOS  : nm -gU                          (Mach-O external defined symbols)
#   * Windows: llvm-nm --extern-only --defined-only, or dumpbin /EXPORTS
#              (PE/COFF export table)
#
# Usage:
#   ci/check-no-alloc-exports.sh <wheel-or-dir-or-lib> [more ...]
# Accepts a repaired .whl (unzipped + scanned), a directory (scanned for shared
# libs), or individual .so/.dylib/.pyd/.dll files. Designed to be chained after
# the platform repair step (auditwheel / delocate / delvewheel), scanning the
# REPAIRED wheel.

set -euo pipefail

# Forbidden EXPORTED symbols (host interposition). We match the C malloc family
# and the C++-mangled operator new/delete.
#
# Itanium ABI (Linux ELF / macOS Mach-O, where macOS PREFIXES an underscore):
#   _Znwm _Znwj _Znam _Znaj        operator new / new[]
#   _ZdlPv _ZdaPv                  operator delete / delete[]
# The C family: malloc/free/calloc/realloc/... (bare, or macOS-underscored).
#
# MSVC mangling (Windows PE) for replaceable operator new/delete:
#   ??2@YAPEAX_K@Z   operator new        ??_U@YAPEAX_K@Z  operator new[]
#   ??3@YAXPEAX@Z    operator delete     ??_V@YAXPEAX@Z   operator delete[]
# i.e. the export names begin with ??2 / ??3 / ??_U / ??_V. Plus the CRT malloc
# family by name.
#
# We DELIBERATELY exclude the mi_* exports (mimalloc's public API) -- they are
# matched by neither the malloc-family nor the operator-new patterns below.
ALLOC_REGEX_ITANIUM='^_?(malloc|free|calloc|realloc|reallocarray|posix_memalign|aligned_alloc|memalign|valloc|pvalloc|_Znw[mj].*|_Zna[mj].*|_ZdlPv.*|_ZdaPv.*)$'
ALLOC_REGEX_MSVC='^(malloc|free|calloc|realloc|_aligned_malloc|_aligned_free|\?\?2@|\?\?3@|\?\?_U@|\?\?_V@)'

_os="$(uname -s 2>/dev/null || echo unknown)"

# Resolve a Windows symbol-dumper once (llvm-nm preferred; dumpbin fallback).
WIN_NM=""
if command -v llvm-nm >/dev/null 2>&1; then
  WIN_NM="llvm-nm"
elif command -v nm >/dev/null 2>&1; then
  # LLVM/MSYS nm can read COFF/PE; GNU nm generally cannot, but try it.
  WIN_NM="nm"
fi

# Print the exported (defined, external) symbol NAMES of a library, one per line,
# choosing the right tool for the file/platform.
dump_exports() {
  local lib="$1"
  case "$lib" in
    *.dll|*.DLL|*.pyd|*.PYD)
      if [[ -n "$WIN_NM" ]]; then
        "$WIN_NM" --extern-only --defined-only "$lib" 2>/dev/null | awk '{print $NF}'
      elif command -v dumpbin >/dev/null 2>&1; then
        # dumpbin /EXPORTS: the export name is the last column of the numbered
        # ordinal/RVA/name rows; grab tokens that look like symbols.
        dumpbin //EXPORTS "$lib" 2>/dev/null | awk 'NF>=4 && $1 ~ /^[0-9]+$/ {print $NF}'
      fi
      ;;
    *.dylib|*.so|*.so.*)
      if [[ "$_os" == "Darwin" ]]; then
        # -g external only, -U defined only (no undefined imports).
        nm -gU "$lib" 2>/dev/null | awk '{print $NF}'
      else
        nm -D --defined-only "$lib" 2>/dev/null | awk '{print $NF}'
      fi
      ;;
    *)
      # Default by host OS.
      if [[ "$_os" == "Darwin" ]]; then
        nm -gU "$lib" 2>/dev/null | awk '{print $NF}'
      else
        nm -D --defined-only "$lib" 2>/dev/null | awk '{print $NF}'
      fi
      ;;
  esac
}

scan_lib() {
  local lib="$1"
  local regex="$ALLOC_REGEX_ITANIUM"
  case "$lib" in
    *.dll|*.DLL|*.pyd|*.PYD) regex="$ALLOC_REGEX_MSVC" ;;
  esac
  local hits
  hits="$(dump_exports "$lib" | grep -E "$regex" || true)"
  if [[ -n "$hits" ]]; then
    echo "ERROR: $lib EXPORTS allocator symbol(s) (host interposition!):" >&2
    echo "$hits" | sed 's/^/    /' >&2
    return 1
  fi
  return 0
}

collect_and_scan_dir() {
  local dir="$1"
  local rc=0
  local found=0
  while IFS= read -r -d '' lib; do
    found=1
    scan_lib "$lib" || rc=1
  done < <(find "$dir" -type f \( \
      -name '*.so' -o -name '*.so.*' \
      -o -name '*.dylib' \
      -o -name '*.dll' -o -name '*.DLL' \
      -o -name '*.pyd' -o -name '*.PYD' \) -print0)
  if [[ "$found" -eq 0 ]]; then
    echo "WARNING: no shared libraries found under $dir to scan." >&2
  fi
  return "$rc"
}

main() {
  if [[ "$#" -eq 0 ]]; then
    echo "usage: $0 <wheel|dir|lib> [...]" >&2
    exit 2
  fi
  local rc=0
  local tmp
  for arg in "$@"; do
    case "$arg" in
      *.whl)
        tmp="$(mktemp -d)"
        unzip -q -o "$arg" -d "$tmp"
        collect_and_scan_dir "$tmp" || rc=1
        rm -rf "$tmp"
        ;;
      *.so|*.so.*|*.dylib|*.dll|*.DLL|*.pyd|*.PYD)
        scan_lib "$arg" || rc=1
        ;;
      *)
        if [[ -d "$arg" ]]; then
          collect_and_scan_dir "$arg" || rc=1
        else
          echo "WARNING: skipping unrecognized argument: $arg" >&2
        fi
        ;;
    esac
  done
  if [[ "$rc" -ne 0 ]]; then
    echo "no-alloc-exports gate FAILED: fvtk must not export allocator symbols." >&2
    exit 1
  fi
  echo "no-alloc-exports gate OK: no exported malloc/free/operator new/delete in fvtk libraries (mi_* C API is allowed)."
}

main "$@"
