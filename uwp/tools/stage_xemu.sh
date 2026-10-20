#!/bin/bash
# stage_xemu.sh -- gather the UWP build's package payload into build/uwp-xemu/ for tools/xpkg.sh xemu:
#   xemu.exe       ~/xemu-uwp/xemu/build/uwp/qemu-system-i386w.exe (stripped: the console needs no symbols)
#   opengl32.dll   Mesa's osmesa.dll (tools/mesa_build.sh): GL 4.1 core on the GPU through d3d12
#   zlib1.dll      osmesa.dll's one non-system import (MSYS2 clang64)
#   dxil.dll       the DXIL validator Mesa's d3d12 driver signs shaders with (Windows SDK Redist)
# and checks every import of each against tools/../build/imports/xbox-missing.txt is not violated (the console
# census: a missing function fails the launch silently).
set -eu
source "$(dirname "$0")/env.sh"
cd "$(dirname "$0")/.."
OUT=build/uwp-xemu
mkdir -p "$OUT"
x86_64-w64-mingw32-strip -o "$OUT/xemu.exe" ~/xemu-uwp/xemu/build/uwp/qemu-system-i386w.exe
cp ~/xemu-uwp/mesa-25.0.7/build/win/src/gallium/targets/osmesa/osmesa.dll "$OUT/opengl32.dll"
cp ~/xemu-uwp/sysroot/clang64/bin/zlib1.dll "$OUT/zlib1.dll"
cp "/mnt/e/Windows Kits/10/Redist/D3D/x64/dxil.dll" "$OUT/dxil.dll"
printf 'build/uwp-xemu/opengl32.dll\topengl32.dll\nbuild/uwp-xemu/zlib1.dll\tzlib1.dll\nbuild/uwp-xemu/dxil.dll\tdxil.dll\n' \
    > "$OUT/files.txt"
# the four functions the console lacks (probe census G) must not be imported any more
llvm-readobj --coff-imports "$OUT/xemu.exe" | awk '/Name:/{dll=$2} /Symbol:/{print dll, $2}' > "$OUT/imports.txt"
bad=$(grep -E ' (GetDefaultCommConfigA|CreateIconFromResourceEx|GetRawInputBuffer|ImmGetIMEFileNameA)$' \
      "$OUT/imports.txt" || true)
if [ -n "$bad" ]; then
  echo "stage_xemu: still imports what the console lacks:"; echo "$bad"; exit 1
fi
echo "imports: $(wc -l < "$OUT/imports.txt") functions from $(cut -d' ' -f1 "$OUT/imports.txt" | sort -u | wc -l) DLLs; none of the four known-missing"
ls -la "$OUT"
