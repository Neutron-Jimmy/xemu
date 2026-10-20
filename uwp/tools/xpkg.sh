#!/bin/bash
# xpkg.sh -- an exe (plus extra files) as a signed .msix for the Xbox (Developer Mode, Device Portal install).
#
#   tools/xpkg.sh probe     -> build/uwp/probe/XemuProbe_<ver>.msix   (build/probe/jitprobe.exe)
#   tools/xpkg.sh xemu      -> build/uwp/xemu/Xemu_<ver>.msix        (build/uwp-xemu/xemu.exe + its files)
#
# Adapted from ~/nc2-port-x64 tools/uwp_package.sh: the exe is marked AppContainer, a self-signed dev certificate
# (build/uwp/xemudev.*) signs it, and every run bumps the version so an install is an update (LocalState kept).
# The manifest declares codeGeneration: without it the ...FromApp calls refuse every execute protection.
set -eu
set -o pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
KITS="${XEMU_WINKITS:-/mnt/e/Windows Kits/10/bin/10.0.26100.0/x64}"
PUBLISHER="CN=xemu UWP Dev"
W() { wslpath -w "$1"; }
case "${1:-}" in
probe) NAME=XemuProbe; IDN=XemuUWP.JitProbe; DISP="xemu JIT probe"; EXE=build/probe/jitprobe.exe; EXTRA=build/probe/files.txt ;;
xemu)  NAME=Xemu; IDN=XemuUWP.Xemu; DISP="xemu"; EXE=build/uwp-xemu/xemu.exe; EXTRA=build/uwp-xemu/files.txt ;;
*) sed -n '2,8p' "$0"; exit 2 ;;
esac
OUT=$ROOT/build/uwp/$1
CERT=$ROOT/build/uwp
mkdir -p "$OUT/pkg/Assets" "$CERT"
[ -f "$EXE" ] || { echo "xpkg: no $EXE"; exit 1; }
N=$(( $(cat "$OUT/version.txt" 2>/dev/null || echo 0) + 1 )); echo $N > "$OUT/version.txt"
VERSION="1.0.$N.0"
EXEN=$(basename "$EXE")
cp "$EXE" "$OUT/pkg/$EXEN"
python3 tools/pe_appcontainer.py "$OUT/pkg/$EXEN"
python3 tools/tiles.py "$OUT/pkg/Assets"
sed -e "s|@IDN@|$IDN|g" -e "s|@PUBLISHER@|$PUBLISHER|g" -e "s|@VERSION@|$VERSION|g" -e "s|@DISP@|$DISP|g" \
    -e "s|@EXE@|$EXEN|g" tools/AppxManifest.in.xml > "$OUT/pkg/AppxManifest.xml"
if [ ! -f "$CERT/xemudev.pfx" ]; then
  openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes -keyout "$CERT/xemudev.key" -out "$CERT/xemudev.pem" \
      -subj "/CN=xemu UWP Dev" -addext "extendedKeyUsage=codeSigning" -addext "keyUsage=digitalSignature" \
      -addext "basicConstraints=CA:FALSE" 2>/dev/null
  openssl pkcs12 -export -out "$CERT/xemudev.pfx" -inkey "$CERT/xemudev.key" -in "$CERT/xemudev.pem" -passout pass:xemudev
  openssl x509 -in "$CERT/xemudev.pem" -outform DER -out "$CERT/xemudev.cer"
fi
MAP="$OUT/mapping.txt"
{
  echo "[Files]"
  echo "\"$(W "$OUT/pkg/AppxManifest.xml")\" \"AppxManifest.xml\""
  echo "\"$(W "$OUT/pkg/$EXEN")\" \"$EXEN\""
  for f in "$OUT"/pkg/Assets/*.png; do echo "\"$(W "$f")\" \"Assets\\$(basename "$f")\""; done
  # EXTRA: lines "SRC<TAB>DEST" (SRC relative to the root, DEST with backslashes)
  if [ -n "$EXTRA" ] && [ -f "$EXTRA" ]; then
    while IFS=$'\t' read -r src dst; do [ -n "$src" ] && echo "\"$(W "$ROOT/$src")\" \"$dst\""; done < "$EXTRA"
  fi
} > "$MAP"
MSIX="$OUT/${NAME}_$VERSION.msix"
rm -f "$MSIX"
"$KITS/makeappx.exe" pack /o /h SHA256 /f "$(W "$MAP")" /p "$(W "$MSIX")" | tail -2
"$KITS/signtool.exe" sign /fd SHA256 /f "$(W "$CERT/xemudev.pfx")" /p xemudev "$(W "$MSIX")" | tail -1
echo "$MSIX"
