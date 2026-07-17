# Legacy BASIC KSS player

> Historical documentation. The archived binaries and build helper now live
> under `msx_old`; the supported DOS2 player is `msx/KSPPLAY.COM`.

`BASICKSS.BAS` and `BASICKSS.BIN` load and play an unmodified `.KSS` or
`.KSSX` file from Disk BASIC. The source file is read as a raw random-access
file; it is not converted to KSP, wrapped in an MSX binary header, or otherwise
modified.

Run from Disk BASIC:

```basic
LOAD "BASICKSS.BAS"
RUN
```

The player needs Disk BASIC, a primary MSX memory mapper with enough 16K
segments for the raw-file staging area (segment 16 onward), the reserved
player/main segments 7-11, and the sound
hardware named by the KSS header. A MoonSound/OPL4 or SCC cartridge must be
inserted separately when the selected KSS requires it. The current loader
supports ordinary KSCC/KSSX files and uncompressed 16K banked data whose
declared bank range does not overlap those reserved segments. KSS files using
8K banked data are rejected with a clear message because that mode requires
the original KSSPLAY cartridge/disk banking contract.

Build or rebuild the binary with:

```sh
python3 msx_old/legacy_basic_kss_player/build_msx_basickss.py
```

The raw-file reader uses sequential `INPUT$` blocks, so the final partial
block is handled without padding or changing the source file.
The program reserves 8K for BASIC strings and forces string-space cleanup
between blocks.
