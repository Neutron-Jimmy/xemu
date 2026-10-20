#!/bin/bash
# build_probe.sh -- probe/jitprobe.c -> build/probe/jitprobe.exe (mingw x86_64 from the NC2 x64 track's toolchain)
set -eu
cd "$(dirname "$0")/.."
TC="${XEMU_MINGW:-$HOME/nc2-port-x64/.toolchain64/root/usr/bin}"
mkdir -p build/probe
"$TC/x86_64-w64-mingw32-gcc" -O1 -g -std=gnu11 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000007 \
    -o build/probe/jitprobe.exe probe/jitprobe.c \
    -static -Wl,--dynamicbase -Wl,--high-entropy-va -lwindowsapp -ld3d11 -lruntimeobject
ls -la build/probe/jitprobe.exe
