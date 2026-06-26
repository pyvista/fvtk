#!/usr/bin/env python
"""Regression: the ghost map must survive reentrant updates during FindObject.

vtkPythonUtil::FindObject() looks up the ghost map and, when it finds an
entry, releases the references that the ghost holds.  If the ghost's weak
pointer has expired, releasing the ghost's dict can run arbitrary Python
code: the dict may hold the last Python references to other wrapped VTK
objects, and tearing those down re-enters vtkPythonUtil::RemoveObjectFromMap,
which both inserts new ghosts into the same map and sweeps expired ghosts out
of it, including the very node FindObject holds an iterator to.  FindObject
must therefore remove the node from the map before releasing any references;
erasing it afterwards uses a freed std::map node and corrupts the heap.

Unfixed, this fails on the first hit iteration with the silent dict-loss mode
(or aborts the interpreter on a heap-corruption build).  The test skips
honestly on allocators that never reuse the freed address.
"""

import gc
import sys

from cvista.vtkCommonCore import vtkCollection, vtkObject


def test_ghost_map_reentrant_erase():
    hits = 0
    attempts = 0
    while attempts < 1000 and hits < 100:
        attempts += 1

        # helper's wrapper will be destroyed mid-cascade; the keeper holds a
        # C++ reference so destroying the wrapper makes a ghost (and thereby
        # sweeps the ghost map).
        helper_keeper = vtkCollection()
        helper = vtkObject()
        helper.tag = attempts
        helper_keeper.AddItem(helper)

        # host's dict holds the last Python reference to helper.
        host_keeper = vtkCollection()
        host = vtkObject()
        host.payload = helper
        host_keeper.AddItem(host)
        host_this = host.__this__
        del helper
        del host

        # On free-threaded builds an explicit collection is needed to ensure
        # the wrapper is actually deallocated here.
        if hasattr(sys, "_is_gil_enabled") and not sys._is_gil_enabled():
            gc.collect()

        # Destroy the host's C++ object: the ghost is now expired and its map
        # key is a freed address.
        host_keeper.RemoveAllItems()

        # If this allocation reuses the host's address, construction finds the
        # expired ghost and releases the ghost's dict.
        probe = vtkObject()
        if probe.__this__ == host_this:
            hits += 1
            # The probe must not inherit the dead host's dict.
            assert not hasattr(probe, "payload")
            # helper was ghosted during the cascade; resurrect it and check
            # that its ghost (and dict) survived the sweep.
            resurrected = helper_keeper.GetItemAsObject(0)
            assert resurrected.tag == attempts
            del resurrected

        del probe
        helper_keeper.RemoveAllItems()

    if hits == 0:
        import pytest

        pytest.skip(
            "the allocator never reused the freed address, so the reentrant "
            "ghost path was not exercised"
        )
