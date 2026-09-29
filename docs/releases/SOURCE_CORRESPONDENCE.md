# Which source each published release was built from

Each GitHub release of this project carries one ZIP. This page records, for every
release, the commit whose tree the ZIP's `eboot.bin` was built from, the revisions
of what was linked into it, and where the release's tag points. It was established
on 2026-09-29 from the published assets (downloaded and hashed), their upload
times, the repositories' public push events and this repository's history. What
could not be established is said so.

**The tags do not always name the built commit.** The releases were created with
`gh release create` against `main`, and GitHub made each tag from whatever `main`
held on GitHub at that moment. Three times the build was pushed afterwards, so the
tag names an older commit than the one built. Tags are not moved (published history
is not rewritten); the table below is the correction, and docs/RELEASING.md is the
procedure that prevents it.

## The releases

| Release | Asset (sha256) | Tag points to | Built from | Evidence for "built from" |
| --- | --- | --- | --- | --- |
| v0.1.0-alpha.1 | `PS5_RetroArch-v0.1.0-alpha.1.zip` `757a6ab774b990638a71e8dbe80deb6d66e799b53c4b52ac4cb504280c3388c4` | `a18cec6` | `b1dced9` | The ZIP was uploaded at 2026-09-20 01:02 UTC, after the release was created; its `eboot.bin` build identity `e11018a7…` is recorded in `b1dced9` (docs/THUMBNAIL_INPUT_DEFAULTS.md), pushed at 01:01 UTC. |
| v0.2.0-alpha.2 | `…v0.2.0-alpha.2.zip` `5f611cb32e8212fb8a0b63f32c2329bfecd828dfd4434949a9a1366f332b13f2` | `d0a815b` | `d0a815b` | Pushed at 13:49 UTC, release at 13:53 UTC; no later commit before the upload. The build identity `cce8e166…` is not recorded in the repository. |
| v0.3.0-alpha.3 | `…v0.3.0-alpha.3.zip` `d313b30dba97ed82277ee74a495801306a64a24fe365e9aba464c8df25da86eb` | `18dc105` | a tree at or after `c1d9f15`, most likely `6c7f418` | The ZIP's `eboot.bin` contains `src/permissions_ps5.cpp`'s log string ("permissions: /app0 checked=…"), which `c1d9f15` added after the tag. `6c7f418` is the last commit before the upload; the build identity `ad98b960…` is not recorded. |
| v0.4.0-alpha.4 | `…v0.4.0-alpha.4.zip` `093fd883bbff63237486d3f7b9b1cd29c9a0d3142e42bf53367861b72757b144` | `52009a9` | `52009a9` | Build identity `870bd1bb…` is recorded in `054ab98`, an ancestor of the tag; the next commit (`a9452a6`) is documentation. |
| v0.5.0-alpha.5 | `…v0.5.0-alpha.5.zip` `aa85d74e224bab3f0dc414c1176f3aa04ddf02b5d86d522b218d6c9c30e769de` | `1ae4a1a` (2026-09-26) | `72847d6` | The tag's tree cannot link RADV (`tools/build-title.sh` there has no RADV path; it arrived in `f932c82` on 2026-09-28). Build identity `f96bdb0e…` is recorded in `72847d6`, pushed at 12:30 UTC, five minutes after the release. |

Every "built from" commit above is now on the public `main`.

## What was linked in, per release

| Release | RetroArch | Cores (pinned upstream or fork revision) | Graphics driver | SDK |
| --- | --- | --- | --- | --- |
| alpha.1 | 1.22.2 `69a4f0e` + patches | FCEUmm `236ccdf`, mGBA `7a12d6d`, Snes9x `fae2fea`, FBNeo `6bb3167`, Genesis Plus GX `c2838c7` | ps5vk from `../PS5_Vulkan`, revision not recorded; built before PS5_Vulkan's public history begins (2026-09-20) | ps5-payload-dev v0.42 release |
| alpha.2 | same | + PPSSPP v1.20.4 `fa50bb1` | ps5vk, revision not recorded (PS5_Vulkan `085aac6` was pushed 5 minutes before the release) | v0.42 release |
| alpha.3 | same | + Dolphin `c6630001` (libretro/dolphin) | ps5vk, revision not recorded; the latest driver commit before the upload (`81b21d8`) was pushed 2026-09-26 01:19 UTC, after the release | v0.42 release |
| alpha.4 | same | + LRPS2 `6d14775` (PS5_LRPS2) | ps5vk `40e9d30` (release notes); pushed 2026-09-28 04:06 UTC, after the release | PS5_PayloadSDK `a110320`; the fork was published 2026-09-28 |
| alpha.5 | same | same eight | RADV from PS5_Mesa `cedb774`, built by PS5_Vulkan `5d8f37d` | PS5_PayloadSDK `95c08f2` |

The upstream cores' pins did not change between releases. The mGBA and Genesis Plus
GX cores built on 2026-09-29 from these pins are byte-identical to the ones in all
five ZIPs, and FCEUmm's to alpha.2–alpha.5, which confirms those pins.

## Gaps these releases still carry

- **Licence texts.** None of the five ZIPs contains the GPL, the MPL, or the
  Snes9x, FBNeo or Genesis Plus GX licences, or the OFL of PPSSPP's fonts. Builds
  from now on stage them in `licenses/` (tools/stage-notices.py). The published
  ZIPs are unchanged.
- **Source not offered beside the binary.** The release pages link the
  repositories but carry no source archives; several cores come from upstream
  repositories this project does not control. tools/source-bundle.py makes the
  archives; attaching them to a release is the maintainer's decision.
- **alpha.1's driver.** The ps5vk revision linked into alpha.1 predates PS5_Vulkan's
  public history, so its source is not published anywhere today.
- **Driver revisions of alpha.2 and alpha.3** are not recorded.

## Proposed "Source" section for each release's notes

Not applied: editing a published release is the maintainer's call.

```markdown
## Source

This ZIP was built from PS5_RetroArch <BUILT-FROM COMMIT> (the tag <TAG> points to
<TAG COMMIT>, an earlier commit; see docs/releases/SOURCE_CORRESPONDENCE.md). Every
component, its licence and its source revision are listed there. The licence texts
for this release are in <link to the licence bundle attached to this release>.
```
