#!/bin/bash
# release.sh -- the downloadable release: build/release/Xemu-UWP-<version>.zip holding the signed .msix, the
# self-signed certificate and INSTALL.txt, plus build/release/notes-<version>.md for the GitHub release page.
#
#   tools/release.sh        (after the ninja build: stages, packages, zips)
#
# The version is the package's (1.0.N.0, tools/xpkg.sh bumps N), so a newer release always installs as an
# update over an older one. Only the .cer leaves build/uwp: the .key/.pfx/.pem (the signing key) never do.
set -eu
set -o pipefail
cd "$(dirname "$0")/.."
tools/stage_xemu.sh
MSIX=$(tools/xpkg.sh xemu | tail -1)
[ -f "$MSIX" ] || { echo "release: no package"; exit 1; }
VER=$(basename "$MSIX" .msix)
VER=${VER#Xemu_}
OUT=build/release
STAGE=$OUT/Xemu-UWP-$VER
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$MSIX" "$STAGE/"
cp build/uwp/xemudev.cer "$STAGE/"
cat > "$STAGE/INSTALL.txt" <<EOF
Xemu UWP $VER -- xemu v0.8.137 for Xbox in Developer Mode
https://github.com/Neutron-Jimmy/xemu/tree/uwp

1. Open Device Portal (the address is shown in Dev Home on the console).
2. Home -> My games & apps -> Add: choose Xemu_$VER.msix, Next, Start.
   (xemudev.cer is the package's self-signed certificate, for install tools that ask for one.)
3. Copy your MCPX boot ROM, flash BIOS, HDD image and games to the console,
   e.g. into D:\\DevelopmentFiles with Device Portal's File Explorer.
4. Start xemu. Press View + Menu together for xemu's menu; set the files under
   Settings -> System, and load a game with Load Disc.

No BIOS, ROM, HDD image or game is included: you need your own.
EOF
( cd "$OUT" && rm -f "Xemu-UWP-$VER.zip" && python3 -c "
import os, sys, zipfile
d = sys.argv[1]
with zipfile.ZipFile(d + '.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for f in sorted(os.listdir(d)):
        z.write(os.path.join(d, f), os.path.join(d, f))
" "Xemu-UWP-$VER" )
# nothing secret may ride along
if python3 -c "
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
bad = [n for n in names if n.endswith(('.key', '.pfx', '.pem'))]
print('\n'.join(names))
sys.exit(1 if bad else 0)
" "$OUT/Xemu-UWP-$VER.zip"; then :; else echo "release: a key file got into the zip -- stopping"; rm -f "$OUT/Xemu-UWP-$VER.zip"; exit 1; fi
echo "$VER" > "$OUT/latest.txt"
ls -la "$OUT/Xemu-UWP-$VER.zip"
