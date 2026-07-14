#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OPENMSX=${OPENMSX:-/Applications/openMSX.app/Contents/MacOS/openmsx}

exec "$OPENMSX" \
  -machine Panasonic_FS-A1ST \
  -ext moonsound \
  -cart "$SCRIPT_DIR/../dutch_moonsound_veterans/ksp/DMV_5_music.rom" \
  -romtype KonamiSCC \
  -diska "$SCRIPT_DIR"
