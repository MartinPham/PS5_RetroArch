# Releasing

What a published release must carry, and the order that makes the tag, the ZIP and
the source agree. The history behind each rule is docs/releases/SOURCE_CORRESPONDENCE.md.

## What a release carries

1. **The title ZIP**, `PS5_RetroArch-<tag>.zip`, holding `PPSA99169/` with its
   `licenses/` folder: the licence texts each part requires, `components.json`
   (every part's licence and source revision, and the sha256 of every executable
   file) and `README.txt`. `tools/build-title.sh` writes it
   (`tools/stage-notices.py`, from `tooling/notices/components.json`).
2. **The source archives** from `tools/source-bundle.py`, with their `SHA256SUMS` and
   `SOURCES.txt`, attached to the same release. The GPLv3 lets the source sit on
   another server with clear directions (section 6(d)), and GPLv2 counts access from
   the same place as the binary (section 3); attaching the archives satisfies both,
   and it covers the cores this project fetches from repositories it does not
   control. Genesis Plus GX's licence asks for the complete source of a modified
   redistribution, which the archives are.
3. **The notes**, with a "Source" section naming the commit the ZIP was built from
   (it is the tag, see below) and the non-commercial parts (Snes9x, FBNeo, Genesis
   Plus GX): they may not be sold. FBNeo's licence also forbids asking for donations
   for a project that uses its code, so a release page carries no donation link.

## The order

1. **Commit and push everything that is linked in.** PS5_RetroArch itself, and every
   revision it pins or takes from a sibling: PS5_PayloadSDK (`sdk_revision`), each
   core fork, PS5_Vulkan (its `HEAD`) and the PS5_Mesa revision its RADV archive
   records. Check that each is on GitHub before building:

   ```bash
   gh api repos/mihawk-99/PS5_Mesa/commits/<revision> --jq .sha
   ```

2. **Build from the clean tree**, naming the release:

   ```bash
   PS5_RELEASE_TAG=v0.6.0-alpha.6 bash tools/build-title.sh
   bash tools/verify.sh
   python3 tools/check-notices.py --release
   ```

   `--release` fails when any part was built from uncommitted source or has no
   published address.

3. **Make the source archives and the ZIP**:

   ```bash
   python3 tools/source-bundle.py dist/PPSA99169 dist/source-v0.6.0-alpha.6
   (cd dist && zip -r -X PS5_RetroArch-v0.6.0-alpha.6.zip PPSA99169)
   sha256sum dist/PS5_RetroArch-v0.6.0-alpha.6.zip
   ```

   An asset may be at most 2 GiB. The largest archives are PPSSPP (about 560 MB,
   its submodules included), MAME and the parts of LLVM RPCS3 links.

4. **Tag the built commit and push the tag before creating the release**, so GitHub
   does not make a tag from whatever `main` holds:

   ```bash
   git tag -a v0.6.0-alpha.6 -m "PS5 RetroArch v0.6.0-alpha.6" <built commit>
   git push PS5_RetroArch v0.6.0-alpha.6   # this repository's remote for github.com/mihawk-99/PS5_RetroArch
   gh release create v0.6.0-alpha.6 --verify-tag --prerelease \
       dist/PS5_RetroArch-v0.6.0-alpha.6.zip dist/source-v0.6.0-alpha.6/*
   ```

5. **Read it back.** Download the ZIP from the release, compare its sha256 with step
   3, unpack it and run `python3 tools/check-notices.py <unpacked>/PPSA99169`.

## Parts that need a decision before they ship

- **RPCS3** is GPL-2.0-only and is combined with GPL-3.0-or-later code (this port's
  core runtime object is linked into it, and it runs against the title's GPL-3.0
  platform code). Do not ship it in a release until that is resolved; the audit's
  counsel packet sets out the options.
- **Artwork whose origin is not recorded** (`sce_sys/pic0.dds`, `pic1.dds`) should be
  replaced with artwork whose licence is known, or its origin recorded, before the
  next release.
