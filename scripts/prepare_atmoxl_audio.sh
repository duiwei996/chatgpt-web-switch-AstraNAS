#!/usr/bin/env bash
set -euo pipefail

ATMOXL_COMMIT="${ASTRANAS_ATMOXL_COMMIT:-8516930078991664243e07fc0d03b31e6e781eb2}"
ATMOXL_FERTIG_BLOB="${ASTRANAS_ATMOXL_FERTIG_BLOB:-53320441588ddc0b6a647239effe1d6ebfe6b2a5}"
DEST="${1:-romfs/audio/fertig.wav}"
URL="https://raw.githubusercontent.com/dezem/AtmoXL-Titel-Installer/${ATMOXL_COMMIT}/romfs/audio/fertig.wav"

mkdir -p "$(dirname "$DEST")"

valid_sound() {
  [[ -s "$DEST" ]] && [[ "$(git hash-object "$DEST")" == "$ATMOXL_FERTIG_BLOB" ]]
}

if valid_sound; then
  echo "AtmoXL completion sound ready: $DEST"
  exit 0
fi

TMP="${DEST}.tmp.$$"
rm -f "$TMP"
curl --fail --location --retry 3 --retry-delay 2 --connect-timeout 20 --output "$TMP" "$URL"

actual_blob="$(git hash-object "$TMP")"
if [[ "$actual_blob" != "$ATMOXL_FERTIG_BLOB" ]]; then
  echo "AtmoXL completion sound integrity mismatch: expected $ATMOXL_FERTIG_BLOB got $actual_blob" >&2
  rm -f "$TMP"
  exit 2
fi

mv "$TMP" "$DEST"
echo "AtmoXL completion sound ready: $DEST ($actual_blob)"
