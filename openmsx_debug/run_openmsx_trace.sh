#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TRACE_SCRIPT="$SCRIPT_DIR/openmsx_trace.tcl"

if [[ -n "${OPENMSX_BIN:-}" ]]; then
    OPENMSX="$OPENMSX_BIN"
elif [[ -x "/Applications/openMSX.app/Contents/MacOS/openmsx" ]]; then
    OPENMSX="/Applications/openMSX.app/Contents/MacOS/openmsx"
elif command -v openmsx >/dev/null 2>&1; then
    OPENMSX="$(command -v openmsx)"
else
    echo "openMSX was not found." >&2
    echo "Install the macOS openMSX app, or set OPENMSX_BIN=/path/to/openmsx." >&2
    exit 1
fi

MEDIA="${1:-}"
LOG_DIR="${TRACE_LOG_DIR:-$HOME/openmsx-trace}"
mkdir -p "$LOG_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="${TRACE_LOG_FILE:-$LOG_DIR/openmsx-$STAMP.log}"

MAPPER="${TRACE_MAPPER:-konami_scc}"
PC_FIRST="${TRACE_PC_FIRST:-0x0000}"
PC_LAST="${TRACE_PC_LAST:-0xFFFF}"
PC_TRACE="${TRACE_PC:-on}"
DISASM="${TRACE_DISASM:-on}"
if [[ -n "${TRACE_MEM_RANGES:-}" ]]; then
    MEM_RANGES="$TRACE_MEM_RANGES"
else
    MEM_RANGES='{0xC000 0xFFFF}'
fi
if [[ -n "${TRACE_IO_RANGES:-}" ]]; then
    IO_RANGES="$TRACE_IO_RANGES"
else
    IO_RANGES='{}'
fi

case "$MEM_RANGES" in
    "{{"*) MEM_TCL="$MEM_RANGES" ;;
    *)      MEM_TCL="{$MEM_RANGES}" ;;
esac
case "$IO_RANGES" in
    "{{"*|"{}") IO_TCL="$IO_RANGES" ;;
    *)          IO_TCL="{$IO_RANGES}" ;;
esac

ARGS=()

if [[ -n "${OPENMSX_MACHINE:-}" ]]; then
    ARGS+=(-machine "$OPENMSX_MACHINE")
fi

if [[ -n "$MEDIA" ]]; then
    case "$MEDIA" in
        *.[Rr][Oo][Mm]|*.[Mm][Xx]1|*.[Mm][Xx]2)
            ARGS+=(-cart "$MEDIA")
            ;;
        *.[Dd][Ss][Kk]|*.[Ii][Mm][Gg]|*.[Ff][Dd]1|*.[Ff][Dd]2)
            ARGS+=(-diska "$MEDIA")
            ;;
        *)
            echo "Unknown media extension; pass it manually with OPENMSX_EXTRA_ARGS." >&2
            ;;
    esac
fi

if [[ -n "${OPENMSX_EXTRA_ARGS:-}" ]]; then
    # Intentional shell splitting for explicitly supplied extra arguments.
    # shellcheck disable=SC2206
    EXTRA=( ${OPENMSX_EXTRA_ARGS} )
    ARGS+=("${EXTRA[@]}")
fi

START_COMMAND="trace_mapper $MAPPER; trace_pc_range $PC_FIRST $PC_LAST; trace_pc_enabled $PC_TRACE; trace_disasm $DISASM; trace_mem_ranges $MEM_TCL; trace_io_ranges $IO_TCL; trace_start {$LOG_FILE}"

echo "Starting openMSX"
echo "Trace log: $LOG_FILE"
echo "Mapper:    $MAPPER"
echo "PC range:  $PC_FIRST-$PC_LAST"
echo "PC trace:  $PC_TRACE (disassembly: $DISASM)"
echo "RAM writes:$MEM_RANGES"
echo "I/O writes: $IO_RANGES"

exec "$OPENMSX" \
    "${ARGS[@]}" \
    -script "$TRACE_SCRIPT" \
    -command "$START_COMMAND"
