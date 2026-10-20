#!/bin/bash
# xdeploy.sh -- the Xbox side, through Device Portal's REST API (adapted from ~/nc2-port-x64 tools/uwp_deploy.sh).
#
#   tools/xdeploy.sh probe|xemu install       install (or update) the newest package of that kind
#   tools/xdeploy.sh probe|xemu launch        start it
#   tools/xdeploy.sh probe|xemu get FILE      fetch LocalState\FILE to build/uwp/<kind>/FILE (prints it)
#   tools/xdeploy.sh probe|xemu put SRC NAME  copy SRC to LocalState\NAME
#   tools/xdeploy.sh probe|xemu ls            list LocalState
#   tools/xdeploy.sh probe|xemu kill          stop it
#
# XBOX defaults to https://192.168.1.91:11443 (portal auth OFF). CSRF: a request carrying no cookie is sent a
# token, and the HEADER is what the portal checks.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
XBOX="${XBOX:-https://192.168.1.91:11443}"
KIND="${1:-}"
shift || true
case "$KIND" in
probe) FAM=XemuUWP.JitProbe; NAME=XemuProbe ;;
xemu) FAM=XemuUWP.Xemu; NAME=Xemu ;;
*) sed -n '2,10p' "$0"; exit 2 ;;
esac
OUT=$ROOT/build/uwp/$KIND
mkdir -p "$OUT"
JAR=$OUT/portal.cookies
W() { wslpath -w "$1"; }
CURL() { curl.exe -sk --max-time "${TIMEOUT:-60}" -b "$(W "$JAR")" -c "$(W "$JAR")" "$@"; }
die() { echo "xdeploy: $*" >&2; exit 1; }
csrf() {
  curl.exe -sk --max-time 60 -o /dev/null -D - "$XBOX/api/os/info" | tr -d '\r' \
    | sed -n 's/^[Ss]et-[Cc]ookie: *CSRF-Token=\([^;]*\).*/\1/p' | tail -1
}
b64() { printf '%s' "$1" | base64 -w0; }
entry() {
  CURL "$XBOX/api/app/packagemanager/packages" | python3 -c '
import json, sys
for p in json.load(sys.stdin).get("InstalledPackages", []):
    if p.get("PackageFamilyName", "").startswith(sys.argv[1]):
        print(p["PackageFullName"], p.get("PackageRelativeId", ""))' "$FAM"
}
need_pkg() {
  read -r FULL PRAID < <(entry) || true
  [ -n "${FULL:-}" ] || die "$FAM is not installed"
  Q="knownfolderid=LocalAppData&packagefullname=$FULL&path=%5CLocalState"
}
case "${1:-}" in
install)
  MSIX="${2:-$(ls -t "$OUT"/${NAME}_*.msix | head -1)}"
  T=$(csrf)
  [ -n "$T" ] || die "no CSRF token from $XBOX"
  echo "uploading $(basename "$MSIX") ($(du -m "$MSIX" | cut -f1) MB)"
  TIMEOUT=1800 CURL -H "X-CSRF-Token: $T" -w "HTTP %{http_code}\n" \
      -F "$(basename "$MSIX")=@$(W "$MSIX")" -F "xemudev.cer=@$(W "$ROOT/build/uwp/xemudev.cer")" \
      "$XBOX/api/app/packagemanager/package?package=$(basename "$MSIX")"
  for k in $(seq 1 180); do
    code=$(CURL -o "$OUT/state.json" -w "%{http_code}" "$XBOX/api/app/packagemanager/state")
    [ "$code" != 204 ] && break
    sleep 2
  done
  echo "install state (HTTP $code): $(cat "$OUT/state.json" 2>/dev/null)"
  ;;
launch)
  need_pkg
  T=$(csrf)
  CURL -X POST -d "" -H "X-CSRF-Token: $T" -w "HTTP %{http_code}\n" \
      "$XBOX/api/taskmanager/app?appid=$(b64 "$PRAID")&package=$(b64 "$FULL")"
  ;;
kill)
  need_pkg
  T=$(csrf)
  CURL -X DELETE -H "X-CSRF-Token: $T" -w "HTTP %{http_code}\n" "$XBOX/api/taskmanager/app?package=$(b64 "$FULL")"
  ;;
get)
  need_pkg
  code=$(CURL -o "$(W "$OUT/$2")" -w "%{http_code}" "$XBOX/api/filesystem/apps/file?$Q&filename=$2")
  echo "HTTP $code"
  [ "$code" = 200 ] && cat "$OUT/$2"
  ;;
put)
  need_pkg
  T=$(csrf)
  TIMEOUT=3600 CURL -H "X-CSRF-Token: $T" -w "HTTP %{http_code}\n" -o /dev/null \
      -F "$3=@$(W "$2");filename=$3" "$XBOX/api/filesystem/apps/file?$Q"
  ;;
ls)
  need_pkg
  CURL "$XBOX/api/filesystem/apps/files?$Q" | python3 -c '
import json, sys
for f in json.load(sys.stdin).get("Items", []):
    print(f.get("Name"), f.get("FileSize"))'
  ;;
*) sed -n '2,10p' "$0"; exit 2 ;;
esac
