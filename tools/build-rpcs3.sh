#!/usr/bin/env bash
# Cross-build RPCS3's libretro core (PlayStation 3), from my fork ../PS5_RPCS3
# (github.com/mihawk-99/PS5_RPCS3) at its pinned revision (tools/core-fork.sh),
# with the fork's BUILD_LIBRETRO configuration (docs/RPCS3_PORT.md).
#
# Output: build/cores/stage/{cores,info}/rpcs3_libretro.{so,info}; build-title.sh
# stages those in /app0. It links the static LLVM (tools/build-llvm.sh), FFmpeg
# (tools/build-ffmpeg.sh) and libiconv (tools/build-libiconv.sh) this repository
# builds, and loads Vulkan through volk from the frontend's device, so it links
# no Vulkan loader.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
core_name=rpcs3
source "$root/tools/core-stamp.sh"
source "$root/tools/core-fork.sh"
bash "$root/tools/build-llvm.sh"
bash "$root/tools/build-ffmpeg.sh"
bash "$root/tools/build-libiconv.sh"
core_stamp_skip rpcs3 \
    "$root/build/cores/stage/cores/rpcs3_libretro.so" \
    "$root/build/cores/stage/info/rpcs3_libretro.info" \
    "$root/build/cores/stage/system/RPCS3/fonts" \
    "$root/build/cores/stage/system/RPCS3/Icons" \
    "$root/build/cores/stage/system/RPCS3/patches/patch.yml" \
    "$root/build/cores/stage/system/RPCS3/game_configs/config_database.json" \
    -- "$root/tools/build-rpcs3.sh" "$root/tools/core-fork.sh" "$root/tooling/rpcs3" \
    "$root/.deps/native/llvm-ps5/.revision" "$root/.deps/native/ffmpeg-ps5/.stamp" \
    "$root/.deps/native/libiconv-ps5/.stamp"
[[ $# == 0 ]] || { echo "usage: ${0##*/}" >&2; exit 2; }

revision=2ada8e453c592d8764d2801a667f3da415794918  # ../PS5_RPCS3 main
core_fork_setup
core_fork_checkout PS5_RPCS3 "$revision"
core_info="$root/tooling/rpcs3/rpcs3_libretro.info"

# The submodules the libretro configuration builds, and only those: LLVM and
# FFmpeg are this repository's own builds, and Qt, SDL, OpenAL, cubeb, hidapi
# and the rest are left out. RPCS3 names them relative to its GitHub home.
submodules=(
    3rdparty/asmjit/asmjit 3rdparty/glslang/glslang 3rdparty/zlib/zlib
    3rdparty/zstd/zstd 3rdparty/libpng/libpng 3rdparty/yaml-cpp/yaml-cpp
    3rdparty/pugixml 3rdparty/SoundTouch/soundtouch 3rdparty/fusion/fusion
    3rdparty/GPUOpen/VulkanMemoryAllocator 3rdparty/stblib/stb
    3rdparty/wolfssl/wolfssl 3rdparty/curl/curl 3rdparty/protobuf/protobuf
    3rdparty/miniupnp/miniupnp 3rdparty/rtmidi/rtmidi 3rdparty/libusb/libusb
    3rdparty/7zip/7zip 3rdparty/discord-rpc/discord-rpc 3rdparty/OpenAL/openal-soft
    3rdparty/feralinteractive/feralinteractive
)
for path in "${submodules[@]}"; do
    name=$(git -C "$source_dir" config -f .gitmodules --get-regexp '^submodule\..*\.path$' |
        awk -v p="$path" '$2 == p {sub(/^submodule\./, "", $1); sub(/\.path$/, "", $1); print $1}')
    url=$(git -C "$source_dir" config -f .gitmodules "submodule.$name.url")
    [[ $url == ../../* ]] && url="https://github.com/${url#../../}"
    git -C "$source_dir" config "submodule.$name.url" "$url"
done
git -C "$source_dir" submodule update --init --depth 1 --quiet -- "${submodules[@]}"

# Vulkan's headers at the version the console's driver was built with, and volk.
fetch_pinned() {  # name url digest -> the extracted directory in $core_work
    local name=$1 url=$2 digest=$3 archive="$root/.deps/downloads/$1.tar.gz"
    [[ -f $archive ]] || curl --fail --location --retry 3 "$url" -o "$archive"
    printf '%s  %s\n' "$digest" "$archive" | sha256sum --check --status || {
        echo "error: $name archive digest mismatch" >&2; exit 1; }
    rm -rf -- "$core_work/$name"
    mkdir -p "$core_work/$name"
    tar -xzf "$archive" -C "$core_work/$name" --strip-components=1
}
fetch_pinned vulkan-headers-1.4.354 \
    https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.354.tar.gz \
    4ca3606e57728febf8aef097c9d7ba6c52385955bb4a6dc06fcf9fc3d76541de
fetch_pinned volk-1.4.350.1 \
    https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
    078a9411298e4e0f60f5f5398c890783427c25a414619294ca8e69587bbd5eae

llvm="$root/.deps/native/llvm-ps5"
ffmpeg="$root/.deps/native/ffmpeg-ps5/lib"
build="$core_work/build"
cmake -S "$source_dir" -B "$build" -G Ninja \
    ${core_ccache:+-DCMAKE_C_COMPILER_LAUNCHER=$core_ccache -DCMAKE_CXX_COMPILER_LAUNCHER=$core_ccache} \
    -DCMAKE_TOOLCHAIN_FILE="$root/tooling/rpcs3/ps5-toolchain.cmake" \
    -DPS5_CORE_LINK_INPUTS="$core_work/core_cxx_runtime.o" -DPS5_EMPTY_LIBS="$core_work/empty-libs" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRETRO=ON \
    `# zstd's trace hooks are weak imports nothing on the console defines.` \
    -DCMAKE_C_FLAGS="-march=znver2 -DZSTD_TRACE=0" -DCMAKE_CXX_FLAGS="-march=znver2 -DZSTD_TRACE=0" \
    -DUSE_NATIVE_INSTRUCTIONS=OFF -DUSE_LTO=OFF -DUSE_PRECOMPILED_HEADERS=OFF \
    -DWITH_LLVM=ON -DBUILD_LLVM=OFF -DSTATIC_LINK_LLVM=ON -DLLVM_DIR="$llvm/lib/cmake/llvm" \
    -DUSE_FAUDIO=OFF -DUSE_SDL=OFF -DUSE_LIBEVDEV=OFF -DUSE_DISCORD_RPC=OFF -DUSE_GAMEMODE=OFF \
    `# zlib: the pinned build this repository makes for the frontend (libpng's` \
    `# generated configuration cannot see the submodule's headers).` \
    -DUSE_SYSTEM_ZLIB=ON -DZLIB_INCLUDE_DIR="$root/.deps/native/zlib/root/usr/include" \
    -DZLIB_LIBRARY="$root/.deps/native/zlib/root/usr/lib/libz.a" \
    -DUSE_VULKAN=ON -DUSE_SYSTEM_CURL=OFF -DUSE_SYSTEM_OPENCV=OFF -DUSE_SYSTEM_OPENAL=OFF \
    -DUSE_SYSTEM_SDL=OFF -DUSE_SYSTEM_FFMPEG=ON \
    -DFFMPEG_INCLUDE_DIR="$root/.deps/native/ffmpeg-ps5/include" \
    -DFFMPEG_LIBRARIES="$ffmpeg/libavformat.a;$ffmpeg/libavcodec.a;$ffmpeg/libswscale.a;$ffmpeg/libswresample.a;$ffmpeg/libavutil.a" \
    `# cellL10n's character sets: the console's libc has no iconv.` \
    -DIconv_INCLUDE_DIR="$root/.deps/native/libiconv-ps5/include" \
    -DIconv_LIBRARY="$root/.deps/native/libiconv-ps5/lib/libiconv.a" -DIconv_IS_BUILT_IN=OFF \
    -DRPCS3_VULKAN_HEADERS_DIR="$core_work/vulkan-headers-1.4.354" \
    -DRPCS3_VOLK_DIR="$core_work/volk-1.4.350.1" \
    > "$core_work/configure.log" 2>&1 || { tail -40 "$core_work/configure.log" >&2; exit 1; }
cmake --build "$build" --target rpcs3_libretro --parallel "${JOBS:-16}"
built=$(find "$build" -name rpcs3_libretro.so -print -quit)
[[ -n $built ]] || { echo "error: no rpcs3_libretro.so was produced" >&2; exit 2; }
# The typeface of the core's loading screen (rpcs3/libretro/libretro_loader.cpp),
# Inter 4.1 under the SIL Open Font License, with its licence, in
# <system>/RPCS3/fonts. The firmware's own fonts are its fallback.
inter="$root/.deps/downloads/Inter-4.1.zip"
[[ -f $inter ]] || curl --fail --location --retry 3 \
    https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip -o "$inter"
printf '%s  %s\n' 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e "$inter" |
    sha256sum --check --status || { echo "error: Inter archive digest mismatch" >&2; exit 1; }
fonts="$root/build/cores/stage/system/RPCS3/fonts"
rm -rf -- "$fonts"
mkdir -p "$fonts"
unzip -q -j -o "$inter" extras/ttf/Inter-Regular.ttf extras/ttf/Inter-SemiBold.ttf LICENSE.txt -d "$fonts"
mv -- "$fonts/LICENSE.txt" "$fonts/Inter-LICENSE.txt"

# RPCS3's own overlay images (bin/Icons/ui: the pad glyphs, the save and
# loading icons its native dialogs draw), where it looks for them: its folder.
icons="$root/build/cores/stage/system/RPCS3/Icons"
rm -rf -- "$icons"
mkdir -p "$icons"
cp -a -- "$source_dir/bin/Icons/ui" "$icons/ui"

# RPCS3's game patch database and the fork's patches for games it lacks (the
# fork's bin/patches: their sources in SOURCE), where RPCS3 reads them: its
# config folder's patches/. The core turns each game's frame-rate patch on when
# content loads (a core option).
patches="$root/build/cores/stage/system/RPCS3/patches"
rm -rf -- "$patches"
mkdir -p "$patches"
cp -- "$source_dir"/bin/patches/*.yml "$patches/"

# RPCS3's per-game configuration database (the fork's bin/game_configs: its
# source in SOURCE), which the core answers RPCS3's boot with (a core option).
game_configs="$root/build/cores/stage/system/RPCS3/game_configs"
rm -rf -- "$game_configs"
mkdir -p "$game_configs"
cp -- "$source_dir/bin/game_configs/config_database.json" "$game_configs/"

core_fork_stage "$built" "$core_info" "$revision" tools/build-rpcs3.sh \
    tooling/rpcs3/ps5-toolchain.cmake tooling/rpcs3/rpcs3_libretro.info
