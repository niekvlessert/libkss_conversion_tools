#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OPENMSX=${OPENMSX:-/Applications/openMSX.app/Contents/MacOS/openmsx}

exec "$OPENMSX" \
  -machine Panasonic_FS-A1ST \
  -ext scc \
  -diska "$SCRIPT_DIR" \
