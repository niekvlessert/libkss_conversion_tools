# openMSX macOS debug trace setup

This setup is intended for openMSX 21.x. It logs:

- every executed PC in a selected range, optionally with disassembly;
- memory writes in selected ranges;
- MSX memory-mapper writes to I/O ports `0xFC`-`0xFF`;
- common cartridge mapper bank writes.

## Install

Install the official macOS openMSX application, then place these files together
in any directory.

Make the launcher executable:

```bash
chmod +x run_openmsx_trace.sh
```

Run a ROM:

```bash
./run_openmsx_trace.sh /path/to/game.rom
```

Run a disk image:

```bash
./run_openmsx_trace.sh /path/to/disk.dsk
```

Logs are written to:

```text
~/openmsx-trace/
```

## Recommended focused run

Full PC tracing produces enormous files and slows emulation. Restrict it to the
driver/player address range whenever possible:

```bash
TRACE_PC_FIRST=0x4000 \
TRACE_PC_LAST=0xBFFF \
TRACE_MAPPER=konami_scc \
./run_openmsx_trace.sh /path/to/game.rom
```

For a driver in high RAM:

```bash
TRACE_PC_FIRST=0xC000 \
TRACE_PC_LAST=0xFFFF \
TRACE_MEM_RANGES='{0x8000 0xFFFF}' \
./run_openmsx_trace.sh /path/to/software.dsk
```

For suspected MoonSound I/O writes, add an explicit port range after the
range has been confirmed for the machine/player:

```bash
TRACE_PC_FIRST=0x4000 \
TRACE_PC_LAST=0xBFFF \
TRACE_IO_RANGES='{0xC4 0xC7}' \
./run_openmsx_trace.sh /path/to/software.dsk
```

Multiple ranges can be supplied as a Tcl list:

```bash
TRACE_IO_RANGES='{{0xC4 0xC7} {0x7E 0x7F}}' \
./run_openmsx_trace.sh /path/to/software.dsk
```

## Mapper profiles

- `konami_scc`: `5000`, `7000`, `9000`, `B000` mirrored register ranges.
- `konami`: standard Konami 8 KB mapper register ranges.
- `ascii8`: four 8 KB bank registers in `6000`-`7FFF`.
- `ascii16`: two 16 KB bank register ranges.
- `generic`: logs all writes in `4000`-`BFFF` as possible mapper writes.
- `none`: disables cartridge-mapper classification.

The MSX RAM mapper ports `FC`-`FF` are always logged separately as `RAMMAP`.

## Useful environment variables

```text
OPENMSX_BIN          Explicit path to the openmsx executable
OPENMSX_MACHINE      Machine configuration, e.g. Panasonic_FS-A1GT
OPENMSX_EXTRA_ARGS   Additional openMSX command-line arguments
TRACE_LOG_DIR        Output directory
TRACE_LOG_FILE       Exact output filename
TRACE_MAPPER         Mapper profile
TRACE_PC_FIRST       First PC to log
TRACE_PC_LAST        Last PC to log
TRACE_PC             Enable or disable instruction logging, default on
TRACE_DISASM         Enable or disable instruction disassembly, default on
TRACE_MEM_RANGES     Tcl range list, e.g. '{0xC000 0xFFFF}'
TRACE_IO_RANGES      Tcl I/O range list, e.g. '{0xC4 0xC7}'
```

Example with an explicit MSX2 machine:

```bash
OPENMSX_MACHINE=Panasonic_FS-A1ST \
TRACE_MAPPER=ascii8 \
TRACE_PC_FIRST=0x8000 \
TRACE_PC_LAST=0xDFFF \
./run_openmsx_trace.sh game.rom
```

## Interactive use

Load only the Tcl script:

```bash
/Applications/openMSX.app/Contents/MacOS/openmsx \
  -script /path/to/openmsx_trace.tcl
```

Press `F10` for the openMSX console and use:

```tcl
trace_pc_range 0x4000 0xBFFF
trace_pc_enabled off
trace_disasm off
trace_mem_ranges {{0xC000 0xFFFF}}
trace_io_ranges {{0xC4 0xC7}}
trace_mapper konami_scc
trace_disasm on
trace_start
trace_status
trace_stop
```

A custom filename is also accepted:

```tcl
trace_start ~/openmsx-trace/nemesis3.log
```

## Log record types

```text
PC       instruction PC and disassembly
MEM      memory write
RAMMAP   MSX memory mapper port write
CARTMAP  cartridge mapper bank-register write
IO       selected I/O-port write
```

`PORT=` is the decoded low eight-bit hardware port. `FULLPORT=` preserves the
complete Z80 I/O address reported by openMSX; for `OUT (C),A`, its high byte is
the current `B` register and is not part of the device port decode.

For a low-volume ABI trace, disable instruction logging and watch only the
engine work area plus MoonSound ports. Write records still contain the exact
Z80 PC that caused the write:

```bash
TRACE_PC=off \
TRACE_DISASM=off \
TRACE_MEM_RANGES='{0xD900 0xDA20}' \
TRACE_IO_RANGES='{{0xC4 0xC7} {0x7E 0x7F}}' \
./run_openmsx_trace.sh /path/to/software.dsk
```

The `PC=` field on write records is maintained by the instruction callback and
is intended to identify the instruction responsible for the write.
