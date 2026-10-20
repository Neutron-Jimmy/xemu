#!/bin/bash
# mesa_build.sh -- Mesa 25.0.7 (the last series with OSMesa) cross-built with llvm-mingw as osmesa.dll:
# off-screen GL with the d3d12 driver (GL on the console's GPU through D3D12) and softpipe as the fallback.
#
#   tools/mesa_build.sh            configure (once) + build ~/xemu-uwp/mesa-25.0.7/build/win -> osmesa.dll
#
# Patches applied to the tree (idempotent):
#   - the osmesa target also links driver_d3d12: sw_screen_create() prefers d3d12 when it is compiled in
#     (GALLIUM_DRIVER=softpipe or LIBGL_ALWAYS_SOFTWARE=1 picks softpipe)
#   - mesa/xu_wgl.c joins the target, its three wgl exports join osmesa.def: the DLL stands in for opengl32.dll
set -eu
source "$(dirname "$0")/env.sh"
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd ~/xemu-uwp/mesa-25.0.7
T=src/gallium/targets/osmesa
grep -q driver_d3d12 $T/meson.build || sed -i 's/dep_ws2_32, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,/dep_ws2_32, dep_thread, dep_clock, dep_unwind, driver_swrast, driver_d3d12, idep_mesautil,/' $T/meson.build
cp "$HERE/mesa/xu_wgl.c" $T/xu_wgl.c
grep -q xu_wgl $T/meson.build || sed -i "s/  'osmesa_target.c',/  'osmesa_target.c', 'xu_wgl.c',/" $T/meson.build
grep -q wglGetProcAddress $T/osmesa.def.in || printf 'wglGetProcAddress@4\nwglGetCurrentContext@0\nwglGetCurrentDC@0\n' >> $T/osmesa.def.in
grep -n "driver_d3d12\|xu_wgl" $T/meson.build

cat > build-cross.txt <<EOF
[binaries]
c = 'x86_64-w64-mingw32-clang'
cpp = 'x86_64-w64-mingw32-clang++'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'
pkg-config = 'x86_64-w64-mingw32-pkg-config'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
if [ ! -f build/win/build.ninja ]; then
  meson setup build/win --cross-file build-cross.txt --buildtype=release -Db_ndebug=true \
    -Dplatforms=windows -Dgallium-drivers=d3d12,softpipe -Dvulkan-drivers= -Dosmesa=true -Dllvm=disabled \
    -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Dopengl=true -Dshared-glapi=disabled \
    -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false -Dbuild-tests=false -Dzstd=disabled -Dexpat=disabled \
    -Dvideo-codecs= -Dgallium-va=disabled -Dgallium-d3d12-video=disabled -Dgallium-d3d12-graphics=enabled \
    -Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dc_link_args="-static -static-libgcc" -Dcpp_link_args="-static -static-libgcc -static-libstdc++"
fi
ninja -C build/win -j4 src/gallium/targets/osmesa/osmesa.dll
ls -la build/win/src/gallium/targets/osmesa/osmesa.dll
