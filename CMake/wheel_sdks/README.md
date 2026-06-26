# cvista-sdk

## Overview

`cvista-sdk` distributes the cvista SDK (the VTK C++ headers, CMake config, and
Python wrap tools from the same build that produced the `cvista` runtime wheel) as
a first-class Python wheel.

The wheel registers a scikit-build-core `cmake.prefix`
[entry point][scikit-build-core-entrypoint], so a downstream scikit-build-core
project that depends on `cvista-sdk` gets the bundled install tree on
`CMAKE_PREFIX_PATH` automatically and `find_package(VTK CONFIG)` resolves to it.
Install the SDK whose version matches your `cvista` wheel.

```bash
pip install cvista cvista-sdk
```

```cmake
find_package(VTK CONFIG REQUIRED)
```

[scikit-build-core-entrypoint]:
  https://scikit-build-core.readthedocs.io/en/latest/cmakelists.html#finding-other-packages

## License

Distributed under the OSI-approved BSD 3-clause License. See Copyright.txt for
details.
