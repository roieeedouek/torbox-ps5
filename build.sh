#!/usr/bin/env bash
# Builds TorBox for PS5.
#
#   ./build.sh host      Linux build for testing on the PC        -> build/host/torbox-ps5
#   ./build.sh payload   PS5 payload for elfldr (port 9021)       -> build/torbox-ps5.elf
#   ./build.sh title     PS5 title (home-screen tile) PPSA97630   -> build/PPSA97630/
#   ./build.sh package   payload + title + README in a zip        -> build/torbox-ps5-<version>.zip
#   ./build.sh all       all of the above
#
# Toolchains are used where they are (read-only, not copied into this repo):
#   PS5_PAYLOAD_SDK  payload SDK with curl/OpenSSL  (default ../deps/ps5-sdk-full)
#   PS5_DEPS         the deps folder                (default ../deps)
#   CA_BUNDLE        CA certificates to embed       (default /etc/ssl/certs/ca-certificates.crt)
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
deps=${PS5_DEPS:-$(cd -- "$here/.." && pwd)/deps}
overdrive_deps=$(cd -- "$here/.." && pwd)/overdrive-tb-3rd-user/ps5-react/.deps
if [[ -n ${PS5_PAYLOAD_SDK:-} ]]; then
    sdk=$PS5_PAYLOAD_SDK
elif [[ -d $here/ps5-payload-sdk ]]; then
    sdk=$here/ps5-payload-sdk
elif [[ -d $deps/ps5-sdk-full ]]; then
    sdk=$deps/ps5-sdk-full
else
    sdk=$overdrive_deps/sdk/ps5-payload-sdk
fi
if [[ -n ${CA_BUNDLE:-} ]]; then
    ca=$CA_BUNDLE
elif [[ -f /etc/ssl/certs/ca-certificates.crt ]]; then
    ca=/etc/ssl/certs/ca-certificates.crt
else
    ca=/etc/ssl/cert.pem
fi
out=$here/build
version=$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' "$here/src/app.h")
srcs=("$here"/src/*.c)
warn=(-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Wno-format-truncation -Wno-unknown-warning-option)

gen_assets() {
    mkdir -p "$out/gen"
    python3 -I "$here/tools/embed.py" "$here/web" "$ca" "$out/gen/assets.c"
}

build_host() {
    gen_assets
    local curl=${HOST_CURL:-}
    if [[ -z $curl ]]; then
        for candidate in /home/linuxbrew/.linuxbrew/opt/curl /opt/homebrew/opt/curl /usr/local/opt/curl; do
            if [[ -d $candidate ]]; then curl=$candidate; break; fi
        done
    fi
    local cflags=() ldflags=(-lcurl -lpthread)
    if [[ -n $curl && -d $curl ]]; then
        cflags+=(-I"$curl/include")
        ldflags=(-L"$curl/lib" -Wl,-rpath,"$curl/lib" -lcurl -lpthread)
    fi
    mkdir -p "$out/host"
    cc -O2 -g "${warn[@]}" -I"$here/src" "${cflags[@]}" "${srcs[@]}" "$out/gen/assets.c" \
        "${ldflags[@]}" -o "$out/host/torbox-ps5"
    echo "host: $out/host/torbox-ps5"
}

build_payload() {
    gen_assets
    local hb=${PS5_HOMEBREW:-$sdk/target/user/homebrew}
    if [[ ! -f $hb/lib/libcurl.a && -f $overdrive_deps/network-ports/v0.40.2/lib/libcurl.a ]]; then
        hb=$overdrive_deps/network-ports/v0.40.2
    fi
    local ppath=$PATH
    for llvm in /opt/homebrew/opt/llvm@18/bin /usr/local/opt/llvm@18/bin; do
        if [[ -d $llvm ]]; then ppath="$llvm:$ppath"; break; fi
    done
    PATH=$ppath PS5_PAYLOAD_SDK=$sdk "$sdk/bin/prospero-clang" -O2 "${warn[@]}" -DCURL_STATICLIB -I"$here/src" -I"$hb/include" \
        "${srcs[@]}" "$out/gen/assets.c" -L"$hb/lib" -lcurl -lpsl -lssl -lcrypto -lzstd -lz -lpthread \
        -o "$out/torbox-ps5.elf"
    PATH=$ppath "$sdk/bin/llvm-strip" --strip-debug "$out/torbox-ps5.elf" 2>/dev/null || true
    echo "payload: $out/torbox-ps5.elf ($(wc -c < "$out/torbox-ps5.elf" | tr -d ' ') bytes)"
}

build_title() {
    bash "$here/title/build_title.sh" "$out"
}

package() {
    [[ -f $out/torbox-ps5.elf ]] || build_payload
    [[ -f $out/PPSA97630/eboot.bin ]] || build_title
    local pkg=$out/torbox-ps5-$version
    rm -rf "$pkg" "$pkg.zip"
    mkdir -p "$pkg"
    cp "$out/torbox-ps5.elf" "$pkg/"
    cp -r "$out/PPSA97630" "$pkg/"
    cp "$here/README.md" "$pkg/"
    (cd "$out" && python3 -I -m zipfile -c "torbox-ps5-$version.zip" "torbox-ps5-$version")
    echo "package: $pkg.zip"
}

case ${1:-all} in
host) build_host ;;
payload) build_payload ;;
title) build_title ;;
package) package ;;
all) build_host && build_payload && build_title && package ;;
*) echo "usage: $0 host|payload|title|package|all" >&2; exit 2 ;;
esac
