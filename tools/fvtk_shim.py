"""Validation shim: redirect `vtkmodules[.*]` imports to `fvtk[.*]`.

Runs at interpreter startup (sitecustomize is auto-imported), so the redirect is
active before pyvista's own conftest/_vtk lazy-loader imports anything. Stock
`vtk`/`vtkmodules` are UNINSTALLED in this venv, so any import that slips past the
redirect fails loudly (ModuleNotFoundError) instead of silently testing stock VTK
-- we want a loud miss, never a false green.

Unlike the README's minimal find_spec shim, this registers the resolved fvtk
module under the *requested* vtkmodules name (aliases in sys.modules), which the
full pyvista test suite needs (importlib.import_module caches by requested name).
"""
import importlib
import importlib.abc
import importlib.util
import sys


class _FvtkRedirect(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    PREFIX = "vtkmodules"
    TARGET = "fvtk"

    def find_spec(self, name, path=None, target=None):
        if name == self.PREFIX or name.startswith(self.PREFIX + "."):
            return importlib.util.spec_from_loader(name, self)
        return None

    def create_module(self, spec):
        target = self.TARGET + spec.name[len(self.PREFIX):]
        mod = importlib.import_module(target)
        sys.modules[spec.name] = mod  # alias under the requested vtkmodules name
        return mod

    def exec_module(self, module):
        pass  # already executed by import_module in create_module


if not any(isinstance(f, _FvtkRedirect) for f in sys.meta_path):
    sys.meta_path.insert(0, _FvtkRedirect())
