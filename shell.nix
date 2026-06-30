{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    # binutils provides ld.gold, required by the CVISTA_ICF lever
    # (-fuse-ld=gold -Wl,--icf=all). The gcc-wrapper already exposes a gold on
    # PATH, but declaring binutils makes the ICF toolchain an explicit part of
    # the dev/CI environment rather than an incidental gcc-wrapper detail.
    binutils
  ];

  buildInputs = with pkgs; [
    # Python 3.13 to match the pyvista parity reference interpreter. Must NOT be
    # 3.11: VTK's FindPython3 + wrapper-suffix detection use the shell `python3`,
    # so a 3.11 here poisons the module ABI tag (cpython-311 in a cp313 wheel)
    # and skips Python wrapping for part of the module set.
    python313
    python313Packages.setuptools
    python313Packages.pip

    gcc
    tbb

    xorg.libX11
    xorg.libXcursor
    xorg.libXext
    xorg.libXfixes
    xorg.libXrandr
    xorg.libXrender
    xorg.libXt
    xorg.libXinerama

    libGL
    mesa

    # Runtime libs the wheel's smoke/parity step needs when importing the built
    # modules + numpy outside the build (libz for numpy, libstdc++ comes from gcc
    # above). Harmless for the build itself.
    zlib
  ];

  shellHook = ''
    export CMAKE_PREFIX_PATH=${pkgs.lib.makeSearchPath "lib/cmake" [
      pkgs.tbb
      pkgs.xorg.libX11
      pkgs.xorg.libXext
      pkgs.xorg.libXrandr
      pkgs.xorg.libXinerama
      pkgs.mesa
      pkgs.libGL
    ]}
  '';
}
