#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# ../PS5_PayloadSDK, the payload SDK fork, with the commit tools/setup-native-dependencies.sh
# pins (it exports that commit with git archive): just that commit, fetched, beside this
# checkout.
#
#   .github/scripts/fetch-sdk-fork.sh
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
fork=$root/../PS5_PayloadSDK
revision=$(sed -n 's/^sdk_revision=\([0-9a-f]\{40\}\).*/\1/p' "$root/tools/setup-native-dependencies.sh")
[[ -n $revision ]] || { echo "fetch-sdk-fork: no sdk_revision in tools/setup-native-dependencies.sh" >&2; exit 2; }
if ! git -C "$fork" cat-file -e "$revision^{commit}" 2>/dev/null; then
    [[ -d $fork/.git ]] || {
        git init -q "$fork"
        git -C "$fork" remote add origin https://github.com/mihawk-99/PS5_PayloadSDK.git
    }
    git -C "$fork" fetch -q --depth 1 origin "$revision"
fi
echo "==> [sdk] PS5_PayloadSDK ${revision:0:12} beside $root"
