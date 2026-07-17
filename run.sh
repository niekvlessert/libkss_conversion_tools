#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OPENMSX=${OPENMSX:-/Applications/openMSX.app/Contents/MacOS/openmsx}

exec "$OPENMSX" \
-machine Panasonic_FS-A1ST \
  -ext msxdos2 -extb scc \
  -diska "$SCRIPT_DIR/msx" \
  -script "$SCRIPT_DIR/../msx_debug_tcl/run_kssplay.tcl"
