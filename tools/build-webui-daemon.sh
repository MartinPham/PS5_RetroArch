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
# HTTP and HTTPS through libcurl over mbedTLS 3.6 (tools/build-curl.sh): Sony's SSL
# failed every handshake from the payload (src/scraper_http.h). Its headers come first,
# so the updater's SHA-256 is mbedTLS 3.6's too: RetroArch's 2.x one (in libupdate.a)
# has the same names over other structures, and is left out below.
bash "$root/tools/build-curl.sh" >/dev/null
curl="$root/.deps/native/curl-ps5"
includes=(-I"$curl/include" -I"$root/src" -I"$root/.deps/webui/libmicrohttpd-1.0.10/src/include"
    -I"$root/.deps/native/zlib/zlib-1.3.2" -I"$root/.deps/native/zlib/zlib-1.3.2/contrib/minizip")
objects=()
for source in daemon/webui_daemon src/webui_ps5 src/webui_transfer src/webui_update src/scraper src/scraper_http; do
    "$sdk/bin/prospero-clang++" -std=c++17 -O2 -Wall -Werror -DPS5_SCRAPER_CURL "${includes[@]}" \
        -c "$root/$source.cpp" -o "$build/${source##*/}.o"
    objects+=("$build/${source##*/}.o")
done
# The shared game library (src/ps5_library.c), which the scraper reads, as every frontend.
"$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Werror -c "$root/src/ps5_library.c" -o "$build/ps5_library.o"
objects+=("$build/ps5_library.o")
# The daemon's own files are 0777, as the title's (daemon/webui_daemon.cpp).
# The updater's zip reading only, from libupdate.a (its SHA-256 is mbedTLS 3.6's).
update_build=$(dirname "$update")
"$sdk/bin/prospero-clang++" -o "$build/ps5-retroarch-webui.elf" "${objects[@]}" "$http" \
    "$update_build/ioapi.o" "$update_build/unzip.o" \
    "$curl/lib/libcurl.a" "$curl/lib/libmbedtls.a" "$curl/lib/libmbedx509.a" "$curl/lib/libmbedcrypto.a" \
    "$root/.deps/native/zlib/root/usr/lib/libz.a" \
    -Wl,--wrap=mkdir -Wl,--wrap=open -Wl,--wrap=fopen -lSceNet
printf '%s\n' "$build/ps5-retroarch-webui.elf"
