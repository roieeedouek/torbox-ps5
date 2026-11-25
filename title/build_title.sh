#!/usr/bin/env bash
# Builds the PPSA97630 title (the home-screen tile that starts the server payload).
#
#   title/build_title.sh OUT_DIR       -> OUT_DIR/PPSA97630/
#
# Uses, read-only and in place (GPL-3.0 tooling, not copied into this repo):
#   $PS5_DEPS/ps5-vulkan                 native tool (ELF -> signed fake SELF), app_crt, link recipe, libc.prx
#   $PS5_DEPS/ps5-vulkan-template/...    the payload SDK revision titles link against (PS5_SDK_DIR)
#   LLVM_CONFIG                          a llvm-config whose --bindir has LLD 22 (the recipe needs it)
# The payload (OUT_DIR/torbox-ps5.elf, from ./build.sh payload) is put into the title folder.
set -euo pipefail
out=$1
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
deps=${PS5_DEPS:-$(cd -- "$here/../.." && pwd)/deps}
vulkan=${PS5_VULKAN_DIR:-$deps/ps5-vulkan}
sdk=${PS5_SDK_DIR:-$deps/ps5-vulkan-template/.deps/native/ps5-payload-sdk}
archive=$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a
shim=$(cd -- "$here/../.." && pwd)/source-recomp/build/ps5-toolchain/llvm-config
[[ -n ${LLVM_CONFIG:-} || ! -x $shim ]] || export LLVM_CONFIG=$shim
tool=$vulkan/build/host/ps5-native-tool
native=$vulkan/tooling/native
work=$out/title-work
title=$out/PPSA97630
[[ -f $out/torbox-ps5.elf ]] || { echo "build the payload first (./build.sh payload)" >&2; exit 1; }
for f in "$tool" "$sdk/bin/prospero-lld" "$native/app_crt.cpp" "$vulkan/runtime/libc.prx"; do
    [[ -e $f ]] || { echo "missing $f (bootstrap deps/ps5-vulkan first)" >&2; exit 1; }
done
rm -rf "$work"
mkdir -p "$work/obj" "$work/root/build"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }

cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"
cc -std=c11 -O2 -Wall -ffunction-sections -fdata-sections -c "$here/launcher.c" -o "$work/obj/launcher.o"

# The recipe writes a version-script map under ROOT/build: give it a root in our build folder whose tooling is
# the real one, so nothing is written into deps/.
ln -s "$vulkan/tooling" "$work/root/tooling"
# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$work/root" "$sdk" "$archive" || exit 2
# This title has no GPU code: keep the recipe's C/C++ runtime and platform layer, drop the RADV archive.
inputs=()
for f in "${radv_link_inputs[@]}"; do
    [[ $f == "$archive" || $f == --whole-archive || $f == --no-whole-archive ]] || inputs+=("$f")
done
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr --gc-sections "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL -e _start -o "$work/launcher.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/launcher.o" "${inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so

# Imports no module loaded by a title exports would be NULL at run time (libkernel_sys and
# libScePosixForWebKit are not loaded by titles).
null_imports=$(comm -23 \
    <("$sdk/bin/llvm-nm" -D --undefined-only "$work/launcher.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$sdk"/target/lib/*.so; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    echo "imports that would be NULL in a title: $null_imports" >&2
    exit 1
fi

"$tool" link --in "$work/launcher.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
rm -rf "$title"
mkdir -p "$title/sce_sys" "$title/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$title/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$title/eboot.bin" > /dev/null
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$title/sce_module/libc.prx"
cp "$here/param.json" "$title/sce_sys/param.json"
python3 -I "$here/make_icon.py" "$title/sce_sys/icon0.png"
cp "$out/torbox-ps5.elf" "$title/torbox-ps5.elf"
chmod 755 "$title/eboot.bin" "$title/sce_module/libc.prx"
printf 'title: %s (eboot.bin %s bytes)\n' "$title" "$(stat -c %s "$title/eboot.bin")"
