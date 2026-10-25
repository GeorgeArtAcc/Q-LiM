#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# make release: a dated archive of the source code + checksums (SHA-256) - proof of authorship.
#   releases/qlim-<version>-<date>.tar.gz         - all of the code
#   releases/qlim-<version>-<date>.sha256         - the archive checksum (for a notary / timestamping)
#   releases/qlim-<version>-<date>.files.sha256   - the checksum of every file
#   SIGN=1 make release  -> also a signature with your GPG key (.asc), if you have one
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^#define QLIM_VERSION *"\(.*\)"/\1/p' src/version.h)
YEAR=$(date +%Y)
# the year in the copyright lines: 2026 -> 2026-<now>
if [ "$YEAR" != "2026" ]; then
    grep -rl "Copyright (c) 2026" src tests tests-win tools packaging installer install-icon.sh 2>/dev/null | while read -r f; do
        sed -i "s/Copyright (c) 2026\(-[0-9]\{4\}\)\? Georgi Stanimirov Ivanov/Copyright (c) 2026-$YEAR Georgi Stanimirov Ivanov/" "$f"
    done
fi
STAMP=$(date +%Y%m%d-%H%M)
NAME="qlim-$VER-$STAMP"
mkdir -p releases
FILES=$(ls -d src tests tests-win tools assets third_party packaging installer docs Makefile.linux Makefile.win \
             install-icon.sh LICENSE README.md README.bg.md CHANGELOG.md THIRD_PARTY.md 2>/dev/null)
# shellcheck disable=SC2086
tar --sort=name --exclude='*.o' --exclude='*.d' --exclude='build-*' -czf "releases/$NAME.tar.gz" $FILES
# shellcheck disable=SC2086
find $FILES -type f ! -name '*.o' ! -name '*.d' | sort | xargs sha256sum > "releases/$NAME.files.sha256"
(cd releases && sha256sum "$NAME.tar.gz" > "$NAME.sha256")
echo ">>> Release: releases/$NAME.tar.gz"
cat "releases/$NAME.sha256"
if [ "$SIGN" = "1" ]; then
    if command -v gpg >/dev/null 2>&1; then
        gpg --armor --detach-sign "releases/$NAME.tar.gz" && echo ">>> Signed: releases/$NAME.tar.gz.asc"
    else
        echo ">>> gpg not installed - not signed"
    fi
fi
echo ">>> Keep the .tar.gz and the .sha256 (for a notary date / qualified time stamp)."
