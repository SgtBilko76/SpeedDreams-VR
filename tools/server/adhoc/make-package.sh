#!/bin/bash
# Build the relay package: a static binary that runs on any x86_64 Linux, plus
# the service file, installer and README.
#
#   ./make-package.sh [output dir]      -> <out>/sd-adhoc-relay-linux-x86_64.tar.gz

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/../../../dist}"
NAME="sd-adhoc-relay-linux-x86_64"
STAGE="$(mktemp -d)/$NAME"
mkdir -p "$STAGE" "$OUT"

echo "==> building"
cc -O2 -Wall -Wextra -static -o "$STAGE/sd-adhoc-relay" "$HERE/sd-adhoc-relay.c"
strip "$STAGE/sd-adhoc-relay"

echo "==> testing"
python3 "$HERE/test-relay.py" "$STAGE/sd-adhoc-relay" >/dev/null 2>&1 \
    || { echo "!! relay test failed - run test-relay.py by hand to see why" >&2; exit 1; }

cp "$HERE/sd-adhoc-relay.c" "$HERE/sd-adhoc-relay.service" "$HERE/install.sh" "$HERE/import-records.py" "$HERE/README.md" "$STAGE/"
chmod +x "$STAGE/install.sh"

tar -C "$(dirname "$STAGE")" -czf "$OUT/$NAME.tar.gz" "$NAME"
rm -rf "$(dirname "$STAGE")"
echo "==> $(cd "$OUT" && pwd)/$NAME.tar.gz"
