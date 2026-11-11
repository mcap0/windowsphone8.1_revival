#!/usr/bin/env bash
# Build the battery probe (native ARM exe, runs in the InBoxPowerWDSvc service slot) in Windows-Debugger
# and copy it to share/sonda.exe; tools/esplora "put sonda.exe ..." takes it from there.
set -Eeuo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)

vmssh="$repo_root/tools/vmssh"
cd "$repo_root/batteria"

for dir in sonda; do
    "$vmssh" ssh user@192.168.122.21 \
        "if exist C:\\lumia\\src\\$dir rmdir /s /q C:\\lumia\\src\\$dir"
done

tar --exclude=libs --exclude=obj --exclude=out --exclude=build -cf - \
    -C "$repo_root/batteria" sonda -C "$repo_root/apps" DumbPhone.pfx |
    "$vmssh" ssh user@192.168.122.21 "tar -xf - -C C:/lumia/src"

build_output=$("$vmssh" ssh user@192.168.122.21 powershell -NoProfile \
    -ExecutionPolicy Bypass -File 'C:\lumia\src\sonda\build.ps1' 2>&1) || {
    printf '%s\n' "$build_output" >&2
    exit 1
}
printf '%s\n' "$build_output"
if ! grep -q '^EXE ' <<<"$build_output"; then
    echo "Windows Phone ARM build did not produce an executable" >&2
    exit 1
fi

"$vmssh" scp -q user@192.168.122.21:C:/lumia/src/sonda/out/sonda.exe \
    "$repo_root/share/sonda.exe"
echo "copied sonda.exe to share/"
