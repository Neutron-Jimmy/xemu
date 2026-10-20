# env.sh -- source me (in WSL): the cross toolchain, host tools and pkg-config for the xemu UWP build.
#
#   ~/xemu-uwp/toolchain   llvm-mingw 20261006 (UCRT, clang 23)  -- x86_64-w64-mingw32-clang
#   ~/xemu-uwp/sysroot     MSYS2 clang64 packages (tools/msys2_fetch.py)
#   ~/xemu-uwp/hosttools   pkgconf, bison, flex, m4 (apt-get download, unpacked: no sudo)
#   ~/xemu-uwp/bin         x86_64-w64-mingw32-pkg-config (the sysroot's .pc files)
#   ~/.local/bin           meson, ninja (pip --user)
XU="$HOME/xemu-uwp"
export XEMU_SYSROOT="$XU/sysroot"
HT="$XU/hosttools/usr"
export LD_LIBRARY_PATH="$HT/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export BISON_PKGDATADIR="$HT/share/bison"
export M4="$HT/bin/m4"
export PATH="$XU/bin:$(echo "$XU"/toolchain/llvm-mingw-*/bin):$HT/bin:$HOME/.local/bin:$PATH"
mkdir -p "$XU/bin"
if [ ! -x "$XU/bin/x86_64-w64-mingw32-pkg-config" ]; then
  cat > "$XU/bin/x86_64-w64-mingw32-pkg-config" <<'EOF'
#!/bin/sh
export PKG_CONFIG_SYSROOT_DIR="$XEMU_SYSROOT"
export PKG_CONFIG_LIBDIR="$XEMU_SYSROOT/clang64/lib/pkgconfig:$XEMU_SYSROOT/clang64/share/pkgconfig"
unset PKG_CONFIG_PATH
exec pkgconf --static "$@"
EOF
  chmod +x "$XU/bin/x86_64-w64-mingw32-pkg-config"
fi
