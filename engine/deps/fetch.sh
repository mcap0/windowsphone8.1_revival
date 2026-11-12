#!/usr/bin/env bash
# deps/fetch.sh — download the pinned third-party sources into deps/src (checksums in deps/SHA256SUMS: recorded on
# first download, verified afterwards) and unpack each into deps/src/<name>.
set -Eeuo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$here/src
sums=$here/SHA256SUMS
mkdir -p "$src/archives"
touch "$sums"

# name version url
while read -r name version url; do
    [[ -z $name || $name == \#* ]] && continue
    file=$src/archives/$(basename "$url")
    if [[ ! -f $file ]]; then
        echo "download $name $version"
        curl -fsSL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    sum=$(sha256sum "$file" | cut -d' ' -f1)
    known=$(awk -v f="$(basename "$file")" '$2 == f { print $1 }' "$sums")
    if [[ -z $known ]]; then
        echo "$sum $(basename "$file")" >> "$sums"
    elif [[ $known != "$sum" ]]; then
        echo "checksum mismatch for $file" >&2
        exit 1
    fi
    dest=$src/$name
    if [[ ! -d $dest ]]; then
        tmp=$(mktemp -d "$src/.unpack.XXXX")
        case $file in
            *.zip) unzip -q "$file" -d "$tmp" ;;
            *) tar -xf "$file" -C "$tmp" ;;
        esac
        entries=("$tmp"/*)
        if (( ${#entries[@]} == 1 )) && [[ -d ${entries[0]} ]]; then
            mv "${entries[0]}" "$dest"
            rmdir "$tmp"
        else
            mv "$tmp" "$dest"
        fi
        echo "$version" > "$dest/.version"
    fi
done <<'EOF'
zlib 1.3.2 https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
brotli 1.2.1 https://github.com/google/brotli/archive/refs/tags/v1.2.1.tar.gz
nghttp2 1.70.0 https://github.com/nghttp2/nghttp2/releases/download/v1.70.0/nghttp2-1.70.0.tar.xz
libressl 4.3.3 https://github.com/libressl/portable/releases/download/v4.3.3/libressl-4.3.3.tar.gz
libpsl 0.23.3 https://github.com/rockdaboot/libpsl/releases/download/0.23.3/libpsl-0.23.3.tar.gz
curl 8.23.0 https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.xz
icu 77.1 https://github.com/unicode-org/icu/releases/download/release-77-1/icu4c-77_1-src.tgz
icu-data 77.1 https://github.com/unicode-org/icu/releases/download/release-77-1/icu4c-77_1-data-bin-l.zip
harfbuzz 14.6.0 https://github.com/harfbuzz/harfbuzz/releases/download/14.6.0/harfbuzz-14.6.0.tar.xz
libjpeg-turbo 3.2.0 https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz
libpng 1.6.59 https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.59.tar.gz
libwebp 1.6.0 https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz
libxml2 2.15.4 https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz
libxslt 1.1.45 https://download.gnome.org/sources/libxslt/1.1/libxslt-1.1.45.tar.xz
sqlite 3.54.1 https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
woff2 1.0.2 https://github.com/google/woff2/archive/refs/tags/v1.0.2.tar.gz
EOF
echo "sources ready in $src"
