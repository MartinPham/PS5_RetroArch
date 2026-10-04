# PS5 RetroArch - what EmulationStation reads beside its executable.
# Sourced by tools/build-frontend.sh (es-de) after the executable is signed, with
# $root and $app set. ES-DE finds these through its executable's folder,
# /app0/es-de (frontends/es-de/ps5/main_ps5.cpp gives it that argv[0]):
#   resources/  its fonts, graphics, sounds, shaders and certificates, and
#               systems/unix (FreeBSD's) as ES-DE ships it: its systems' names,
#               themes and extensions, which library_ps5.cpp reads when it writes
#               ES-DE's systems from RetroArch's playlists at each start;
#   themes/     alekfull-nx-es-de, the default on the PS5 (CC BY-NC-SA 2.0: artwork and
#               layouts by fagnerpc, some artwork by ClassicxCola, the ES-DE port by
#               anthonycaccese; README.md inside credits them), without its design
#               sources (.psd, .afdesign); and linear-es-de, ES-DE's own (MIT);
#   LICENSE     ES-DE's own (MIT).
# Its settings, game lists and logs are its own, in /app0/es-de/ES-DE; nothing
# here writes them, so a deploy keeps a user's.
# shellcheck shell=bash
esde_release="$root/.deps/es-de"
esde_app="$app/es-de"
rm -rf -- "$esde_app/resources" "$esde_app/themes"
mkdir -p "$esde_app/resources/systems" "$esde_app/themes"
for resource in "$esde_release"/resources/*; do
    [[ $(basename "$resource") == systems ]] || cp -r "$resource" "$esde_app/resources/"
done
cp -r "$esde_release/resources/systems/unix" "$esde_app/resources/systems/"
cp -r "$esde_release/themes/linear-es-de" "$esde_app/themes/"
mkdir -p "$esde_app/themes/alekfull-nx-es-de"
(cd "$root/.deps/alekfull-nx-es-de" && git ls-files -z -- ':!*.psd' ':!*.afdesign' |
    xargs -0 cp --parents -t "$esde_app/themes/alekfull-nx-es-de")
cp "$esde_release/LICENSE" "$esde_app/LICENSE"
printf '==> [frontend] es-de: resources %s, themes alekfull-nx-es-de %s and linear-es-de %s\n' \
    "$(du -sh "$esde_app/resources" | cut -f1)" "$(du -sh "$esde_app/themes/alekfull-nx-es-de" | cut -f1)" \
    "$(du -sh "$esde_app/themes/linear-es-de" | cut -f1)"
