---
name: module-trimming
description: How fvtk trims VTK down to PyVista's module closure — the three levers (module deny-list, NOWRAP classes, NOCOMPILE classes), the FVTK_KEEP_CLASSES override, closure rules, and how to add, remove, or restore a class or module safely. Load when a PyVista path needs a class fvtk dropped, when shrinking the wheel further, or when a build fails because something is missing.
---

# Module and class trimming

fvtk ships ~84 modules (PyVista's measured closure) out of VTK's ~160, and within those modules
drops classes PyVista never touches. The result is a ~37 MB wheel vs stock's ~120 MB. The full
rationale and measurements are in `docs/build-internals.md` (levers 1–11). This skill is the
operational guide.

## The three levers

All three live in `fvtk-config/`. The class lists are **append-only and closure-closed by
design** — every entry is there because nothing kept refers to it.

1. **Module deny-list** — `fvtk-config/_modules_minimal.cmake`
   `VTK_BUILD_ALL_MODULES OFF`; only PyVista's closure is enabled via WANT/YES. This is ~53
   direct C++ imports plus the IO format readers reached through the object factory plus the
   rendering impl modules (`RenderingContextOpenGL2`, `RenderingGL2PSOpenGL2`).

2. **Lever A — NOWRAP** — `fvtk-config/_nowrap_classes.cmake` (~1,173 classes)
   The C++ compiles, but the Python wrapper is skipped. Closed under header references from kept
   classes plus the `vtkmodules` bundled imports. **Removing an entry is zero-risk** (you only
   add a wrapper back). Adding an entry needs a check that no kept Python path imports it.

3. **Lever B — NOCOMPILE** — `fvtk-config/_nocompile_classes.cmake` (~742 classes)
   Dropped entirely: no compile, no wrapper, no hierarchy. Closed under C++ source references,
   transitive `::New()` bases, and generated ObjectFactory registrations. **Adding an entry
   needs closure analysis** (nothing kept may reference it, directly or via factory). Removing an
   entry is safe.

The promotion hooks for both levers live in `CMake/vtkModule.cmake`.

## Restoring a class (the common case)

A PyVista path needs `vtkFooBar`, but it was trimmed and import fails or a filter is missing.

1. Find where it was dropped:
   ```bash
   grep -rn vtkFooBar fvtk-config/_nowrap_classes.cmake fvtk-config/_nocompile_classes.cmake
   ```
2. **Two ways to restore:**
   - **Permanent** (the right fix when PyVista genuinely needs it): delete the line from the
     list. From `_nowrap_classes.cmake` is zero-risk. From `_nocompile_classes.cmake`, also
     restore any bases / referenced classes it needs so the closure holds, then prove a fresh
     configure + build + import.
   - **Ad-hoc / to test** (no canonical-list edit): pass it at configure time. This is the
     supported override:
     ```bash
     cmake ... -DFVTK_KEEP_CLASSES="vtkFooBar;vtkBazQux"
     ```
     `FVTK_KEEP_CLASSES` subtracts each class from both deny-lists, so it compiles and wraps,
     and leaves the canonical lists untouched. Set via env for `build-fvtk.sh`. (Added in #106.)
3. Validate: configure in a fresh build dir, build, `python smoke-fvtk.py`, then the bitexact
   gate if the class affects a filter path.

## Dropping a new class to shrink further

1. Confirm nothing kept references it: search C++ sources for the type, check for `::New()`
   callers and ObjectFactory registrations, and confirm no kept Python path imports it.
2. NOWRAP first (cheap, reversible). Only move to NOCOMPILE once you have proven the full
   closure, since NOCOMPILE removes the hierarchy too.
3. Add the entry to the appropriate list (append-only).
4. Validate in a **fresh build dir** (a dirty cache hides generate-time breakage): configure,
   build, `smoke-fvtk.py`, then the PyVista regression suite — the bar is zero new failures vs
   stock 9.6.2.

## Enabling a whole module

Edit `fvtk-config/_modules_minimal.cmake` to WANT/YES the module. Watch for the **WANT
silent-drop cascade**: forcing a module to NO silently removes its dependents while configure
still succeeds, so a missing module can surface far from where you turned it off. Re-run a fresh
configure and read the module report.

## Traps (read before extending)

- NOCOMPILE filters **classes**, not files. A class listed in a module's SOURCES or TEMPLATES
  still compiles; do not delete its `.cxx`/`.h` to "help".
- Not every `Testing/` directory is deletable; some carry a `vtk.module` or are referenced at
  the top level.
- Structurally-required disabled modules (MPI, Catalyst, WebGPU, Java, SerializationManager)
  must stay declared even though they are off.
- Source pruning does not change which classes compile (NOCOMPILE already excluded them), so a
  configure + compile + import-smoke is sufficient to prove a prune; the full proof is still the
  PyVista suite + bitexact gate.
