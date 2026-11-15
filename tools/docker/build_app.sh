#!/bin/sh
# Runs inside devkitpro/devkita64. /nx = Sims3-NX (bind), /b = build volume.
# usage: build_app.sh [quick|full]   quick stops after the app's own code and default.xex.
set -eu
MODE=${1:-full}
# GEN: the generated tree. The title update's is /nx/work/tu/app/generated (with TU=1 for post_codegen and its
# own build volume, so switching does not rebuild 280 MB of game code).
# Default: the title update build (the one shipped as sims3.nro since 11-15). TU=0 builds the original game from
# /nx/app/generated; give that its own volume (sims3-build) and the update its own (sims3-build-tu).
TU=${TU:-1}
if [ "$TU" = 1 ]; then GEN=${GEN:-/nx/work/tu/app/generated}; else GEN=${GEN:-/nx/app/generated}; fi
export TU
export DEVKITPRO=/opt/devkitpro
K=/nx/third_party/katamari-nx/katamari-nx-main
mkdir -p /b/src /b/out
if [ ! -f /b/src/.copied ]; then
  rm -rf /b/src/sdk /b/src/tools
  echo "[copy] sdk"
  cp -r "$K/sdk" /b/src/sdk
  mkdir -p /b/src/tools
  cp -r "$K/tools/switch" /b/src/tools/switch
  touch /b/src/.copied
fi
echo "[copy] overlay and app"
cp -r /nx/sdk_overlay/. /b/src/sdk/
mkdir -p /b/src/app
cp /nx/app/CMakeLists.txt /nx/app/icon.jpg /b/src/app/
rm -rf /b/src/app/src
cp -r /nx/app/src /b/src/app/src
echo "#define SIMS3_TITLE_UPDATE ${TU:-0}" > /b/src/app/src/sims3_build.h
sh /nx/tools/docker/build_shaders.sh /b/src/app/src/sims3_ui_shaders.h
if [ ! -f /b/src/app/generated/.copied ] || [ "$GEN/default/codegen.stamp" -nt /b/src/app/generated/.copied ]; then
  rm -rf /b/src/app/generated
  cp -r "$GEN" /b/src/app/generated
  touch /b/src/app/generated/.copied
fi
python3 /nx/tools/post_codegen.py /b/src/app/generated
if [ ! -f /b/out/app/CMakeCache.txt ]; then
  cmake -S /b/src/app -B /b/out/app -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=/b/src/tools/switch/cmake/switch-devkitA64.cmake \
    -DREXSDK_DIR=/b/src/sdk \
    -DPYTHON_EXECUTABLE=/usr/bin/python3 -DPython3_EXECUTABLE=/usr/bin/python3
fi
if [ "$MODE" = quick ]; then
  ninja -C /b/out/app -j 8 -k 0 sims3_default_code CMakeFiles/sims3.dir/src/main.cpp.obj CMakeFiles/sims3.dir/src/deko3d_graphics.cpp.obj
else
  ninja -C /b/out/app -j 7 -k 0 sims3
  sh /nx/tools/docker/check_dupes.sh
  mkdir -p /nx/work/nro
  cp /b/out/app/sims3.nro /nx/work/nro/sims3.nro
  ls -l /b/out/app/sims3.nro /b/out/app/sims3 2>/dev/null || true
fi
echo BUILD_APP_DONE
