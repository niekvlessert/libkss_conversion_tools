# MSX KSP player disk

Mount this directory as a disk in openMSX with Disk BASIC. Then run:

```basic
RUN "KSPPLAY.BAS"
```

The machine-code bootstrap displays progress, starts the embedded `ALMOSEND.KSP` payload, initializes track 0, and drives playback at 60 Hz. The materialized engine and song are independently ZX0-packed inside `KSPPLAY.BIN`; the BIN remains below C000H so Disk BASIC can load it safely. BASIC/BIN version; `ALMOSEND.KSP` is kept on disk as the compact source archive. A MoonSound/OPL4 cartridge with its YRW801 ROM is required.
