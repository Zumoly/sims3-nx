#!/bin/sh
# Runs inside devkitpro/devkita64. /nx = Sims3-NX (bind), /b = build volume.
set -eu
export DEVKITPRO=/opt/devkitpro
K=/nx/third_party/katamari-nx/katamari-nx-main
mkdir -p /b/src /b/out
if [ ! -f /b/src/.copied ]; then
  echo "[copy] sdk"
  cp -r "$K/sdk" /b/src/sdk
  mkdir -p /b/src/tools
  cp -r "$K/tools/switch" /b/src/tools/switch
  touch /b/src/.copied
fi
cd /b
if [ ! -f /b/out/sdk/CMakeCache.txt ]; then
  cmake -S /b/src/sdk -B /b/out/sdk -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=/b/src/tools/switch/cmake/switch-devkitA64.cmake \
    -DPYTHON_EXECUTABLE=/usr/bin/python3 -DPython3_EXECUTABLE=/usr/bin/python3
fi
cmake --build /b/out/sdk -j 10 -- -k 0
echo BUILD_SDK_DONE
