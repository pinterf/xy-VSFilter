#!/bin/sh
# Build the downloadable archives from the staged dist/ folders.
# Produces dist/MultiSubFilter-x64.zip and dist/MultiSubFilter-x86.zip,
# each containing: MultiSubFilter.dll, Install_*.bat, Uninstall_*.bat,
# README.md, License.txt.
#
# Uses PowerShell's Compress-Archive (no zip.exe needed on Windows).
set -e
cd "$(dirname "$0")"

for plat in x64 x86; do
  [ -f "$plat/MultiSubFilter.dll" ] || { echo "missing $plat payload - run package.sh first"; exit 1; }
done

powershell -NoProfile -Command "Compress-Archive -Path 'x64\MultiSubFilter.dll','x64\Install_x64.bat','x64\Uninstall_x64.bat','x64\README.md','x64\License.txt' -DestinationPath 'MultiSubFilter-x64.zip' -Force"
powershell -NoProfile -Command "Compress-Archive -Path 'x86\MultiSubFilter.dll','x86\Install_x86.bat','x86\Uninstall_x86.bat','x86\README.md','x86\License.txt' -DestinationPath 'MultiSubFilter-x86.zip' -Force"

ls -l MultiSubFilter-x64.zip MultiSubFilter-x86.zip
