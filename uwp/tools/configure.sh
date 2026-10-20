#!/bin/bash
# configure.sh [desktop|uwp] -- configure ~/xemu-uwp/xemu for Windows x64 with llvm-mingw, out of tree.
#
#   desktop  build/win    a plain Win64 xemu (the toolchain check, and the PC reference for the UWP build)
#   uwp      build/uwp    the same with -DXEMU_UWP: tools/uwp/ front end, split-wx JIT, windowsapp imports
#
# Then: ninja -C ~/xemu-uwp/xemu/build/<dir> -j4 qemu-system-i386w.exe   (-j4: leave the PC usable)
set -eu
source "$(dirname "$0")/env.sh"
KIND="${1:-desktop}"
cd ~/xemu-uwp/xemu
case "$KIND" in
desktop) DIR=build/win; EXTRA_CFLAGS= ;;
uwp) DIR=build/uwp; EXTRA_CFLAGS="-DXEMU_UWP=1" ;;
*) sed -n '2,8p' "$0"; exit 2 ;;
esac
mkdir -p "$DIR"
cd "$DIR"
# Source file names reach the binary through __FILE__ (assert messages): map the builder's home away, so a
# published build carries ./include/... rather than /home/<user>/xemu-uwp/xemu/include/...
SRCROOT=$(cd ../.. && pwd)
../../configure \
    --cross-prefix=x86_64-w64-mingw32- \
    --cc=x86_64-w64-mingw32-clang --cxx=x86_64-w64-mingw32-clang++ --host-cc=gcc \
    --static \
    --target-list=i386-softmmu \
    --disable-werror \
    --extra-cflags="-DXBOX=1 -Wno-error $EXTRA_CFLAGS -ffile-prefix-map=$SRCROOT/=./ -ffile-prefix-map=$HOME/=~/" \
    --extra-ldflags="-liphlpapi -lcrypt32" \
    "${@:2}"
