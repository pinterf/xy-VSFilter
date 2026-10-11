#!/bin/sh
# Stage distributable binaries into dist/x64 and dist/x86.
# Run after building both platforms for Release.
set -e
cd "$(dirname "$0")/.."

DLL="MultiSubFilter.dll"

stage() {
  plat="$1"; out="$2"
  src="bin/lib_17.0/$plat/Release"
  [ -f "$src/$DLL" ] || { echo "missing $src/$DLL - build $plat Release first"; exit 1; }
  mkdir -p "dist/$out"
  cp "$src/$DLL" "dist/$out/"
  cp License.txt "dist/$out/"
  cp dist/README.md "dist/$out/"
  cp "dist/Install_$out.bat" "dist/Uninstall_$out.bat" "dist/$out/"
  echo "staged dist/$out"
}

stage x64 x64
stage Win32 x86
find dist -maxdepth 2 -type f | sort
