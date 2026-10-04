#!/usr/bin/env bash
# PS5 RetroArch - build an SDL2/OpenGL frontend executable beside eboot.bin.
#
#   tools/build-frontend.sh sdl-probe     frontends/sdl-probe -> dist/<TITLE>/es-de/sdl-probe.bin
#
# Why a second executable. EmulationStation draws with OpenGL, which on this
# console is ../PS5_OpenGL's stack: Mesa, as RADV is, so the two cannot share
# one image. LoadExec starts another executable of the title with its arguments
# (evidence/loadexec-second-image/), so a frontend is its own program in /app0,
# built the way ../PS5_OpenGL builds its SDL2 consumers
# (integration/SDL2/folder.py there): its runtime shims and heap, its linker
# script over this project's base layout, a 256 MiB libc heap, SDL2 from its
# bridge (integration/SDL2/build.py native) and the OpenGL SDK's static archive
# and AGC import stubs. The C runtime, the native module tool and the payload
# SDK are this project's, so the executable and eboot.bin share one libc.prx.
#
# Inputs, overridable:
#   PS5_OPENGL      ../PS5_OpenGL                       the bridge and native-app sources
#   PS5_OPENGL_SDK  $PS5_OPENGL/build/sdk/ps5-opengl-gl46
#   PS5_SDL2        the newest $PS5_OPENGL/build/sdl2-retroarch-*/sdk
# Run after tools/build-title.sh: the title folder it writes is where this goes.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
program=${1:?usage: tools/build-frontend.sh <program in frontends/>}
[[ $program =~ ^[a-z0-9-]+$ && -d $root/frontends/$program ]] ||
    { echo "no frontend program frontends/$program" >&2; exit 2; }

opengl=${PS5_OPENGL:-$root/../PS5_OpenGL}
gl_sdk=${PS5_OPENGL_SDK:-$opengl/build/sdk/ps5-opengl-gl46}
sdl=${PS5_SDL2:-$(ls -d "$opengl"/build/sdl2-retroarch-*/sdk 2>/dev/null | sort | tail -1)}
sdk="$root/.deps/native/ps5-payload-sdk"
native="$root/tooling/native"
tool="$root/build/host/ps5-native-tool"
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/sce_sys/param.json")
app="$root/dist/$title_id"
work="$root/build/frontend/$program"

for input in "$gl_sdk/lib/libPS5OpenGLCore33.a" "$gl_sdk/lib/libSceAgc.so" "$gl_sdk/lib/libSceAgcDriver.so" \
    "$sdl/lib/libSDL2.a" "$opengl/native-app/runtime_shims.c" "$opengl/native-app/app_heap.c" \
    "$opengl/native-app/ps5-pie.ld" "$opengl/native-app/app-symbols.map" "$tool" "$app/eboot.bin"; do
    [[ -e $input ]] || { echo "missing input: $input (build the title, the GL SDK and SDL2 first)" >&2; exit 2; }
done

rm -rf "$work"
mkdir -p "$work/obj" "$work/tooling/native"
cc=(env PS5_PAYLOAD_SDK="$sdk" sh "$root/tooling/prospero-clang18")
common=(-O2 -Wall -Wextra -ffunction-sections -fdata-sections)
"${cc[@]}" -std=c++20 "${common[@]}" -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
"${cc[@]}" -std=c++20 "${common[@]}" -fno-exceptions -fno-rtti -c "$native/app_cpp_runtime.cpp" \
    -o "$work/obj/app_cpp_runtime.o"
for shim in runtime_shims app_heap; do
    "${cc[@]}" -std=c11 "${common[@]}" -c "$opengl/native-app/$shim.c" -o "$work/obj/$shim.o"
done
objects=()
while IFS= read -r -d '' source; do
    object="$work/obj/$(basename "${source%.*}").o"
    case $source in
        *.c) "${cc[@]}" -std=c11 "${common[@]}" -I"$sdl/include" -I"$sdl/include/SDL2" -I"$gl_sdk/include" \
                 -c "$source" -o "$object" ;;
        *) "${cc[@]}" -std=c++17 "${common[@]}" -I"$sdl/include" -I"$sdl/include/SDL2" -I"$gl_sdk/include" \
               -c "$source" -o "$object" ;;
    esac
    objects+=("$object")
done < <(find "$root/frontends/$program" "$root/frontends/common" -type f \( -name '*.c' -o -name '*.cpp' \) \
    -print0 | sort -z)

# ../PS5_OpenGL's linker script includes this project's base layout by that path.
cp "$native/ps5-pie.ld" "$work/tooling/native/ps5-pie-base.ld"
cp "$opengl/native-app/ps5-pie.ld" "$opengl/native-app/app-symbols.map" "$work/tooling/native/"
builtins="$(clang --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
[[ -f $builtins ]] || { echo "no compiler builtins at $builtins" >&2; exit 2; }
(
    cd "$work"
    "$sdk/bin/prospero-lld" -T tooling/native/ps5-pie.ld -L tooling/native --eh-frame-hdr --error-limit=0 \
        --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size \
        --Map="$work/$program.map" --version-script tooling/native/app-symbols.map --exclude-libs=ALL \
        -e _start -o "$work/$program.elf" obj/app_crt.o obj/app_cpp_runtime.o obj/runtime_shims.o obj/app_heap.o \
        "${objects[@]}" -L "$sdk/target/lib" -L "$gl_sdk/lib" \
        --start-group "$sdl/lib/libSDL2.a" "$gl_sdk/lib/libPS5OpenGLCore33.a" \
        "$sdk/target/lib/libunwind.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libc++.a" "$builtins" \
        --end-group --as-needed "$sdk"/target/lib/*.so "$gl_sdk/lib/libSceAgc.so" "$gl_sdk/lib/libSceAgcDriver.so"
)
"$tool" link --in "$work/$program.elf" --out "$work/$program.module.elf" --stub-dir "$sdk/target/lib" \
    --stub "$gl_sdk/lib/libSceAgc.so" --stub "$gl_sdk/lib/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name "$program.elf" --libc-heap-size 0x10000000
mkdir -p "$app/es-de"
"$tool" self --sign --in "$work/$program.module.elf" --out "$app/es-de/$program.bin" --magic 0x1D3D154F
printf '==> [frontend] %s: %s bytes at es-de/%s.bin (SDL2 %s, GL SDK %s)\n' "$program" \
    "$(stat -c %s "$app/es-de/$program.bin")" "$program" "${sdl#"$opengl/"}" "${gl_sdk#"$opengl/"}"
