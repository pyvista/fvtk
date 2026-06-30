from __future__ import annotations

import importlib.metadata

import cvista_sdk as m


def test_version():
    assert importlib.metadata.version("cvista_sdk") == m.__version__
