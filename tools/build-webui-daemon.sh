#!/usr/bin/env bash
# PS5 RetroArch - build the WebUI daemon, a payload the title starts through the
# console's ELF loader (daemon/webui_daemon.cpp, src/webui_link.h): the WebUI's server
# and updater, as eboot.bin carries them, with a main of its own. Prints the ELF.
# Its libraries are eboot.bin's (tools/build-webui-http.sh, tools/build-webui-update.sh).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="$root/.deps/native/ps5-payload-sdk"
export PS5_PAYLOAD_SDK="$sdk" PS5_CLANG=${PS5_CLANG:-clang}
build="$root/build/webui-daemon"
mkdir -p "$build"
http=$(bash "$root/tools/build-webui-http.sh" ps5)
update=$(bash "$root/tools/build-webui-update.sh" ps5)
includes=(-I"$root/src" -I"$root/.deps/webui/libmicrohttpd-1.0.10/src/include"
    -I"$root/.deps/native/zlib/zlib-1.3.2" -I"$root/.deps/native/zlib/zlib-1.3.2/contrib/minizip"
    -I"$root/vendor/retroarch/deps/mbedtls")
objects=()
for source in daemon/webui_daemon src/webui_ps5 src/webui_update; do
    "$sdk/bin/prospero-clang++" -std=c++17 -O2 -Wall -Werror "${includes[@]}" \
        -c "$root/$source.cpp" -o "$build/${source##*/}.o"
    objects+=("$build/${source##*/}.o")
done
# The daemon's own files are 0777, as the title's (daemon/webui_daemon.cpp).
"$sdk/bin/prospero-clang++" -o "$build/ps5-retroarch-webui.elf" "${objects[@]}" "$http" "$update" \
    "$root/.deps/native/zlib/root/usr/lib/libz.a" \
    -Wl,--wrap=mkdir -Wl,--wrap=open -Wl,--wrap=fopen -lSceNet -lSceSsl -lSceHttp2
printf '%s\n' "$build/ps5-retroarch-webui.elf"
