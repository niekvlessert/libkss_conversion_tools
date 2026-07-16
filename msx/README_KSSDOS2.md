# MSX-DOS2 KSS player

`KSSPLAY.COM` reads an unmodified `.KSS` or `.KSSX` file through MSX-DOS2
file handles and stages it in primary RAM-mapper segments. It does not convert,
wrap, or rewrite the KSS file.

Run it from an MSX-DOS2 prompt:

```text
KSSPLAY HAPPY.KSS 0
```

The first argument is the KSS filename and the optional second argument is the
song number, defaulting to song 0. Put `KSSPLAY.COM` and the KSS file in the
same mounted disk directory, or provide a DOS2 path.

For the included F-1 Spirit file, track 54 can be started with:

```text
KSSPLAY F1.KSS 54
```

The loader prints `LOADING KSS` followed by one dot per 16 KiB read, then
prints `KSS LOADED`. The input file is never modified. The verified F-1 Spirit
8K-bank layout is adapted in the staged RAM copy; other generic 8K-bank KSS
files are still rejected.

The loader reserves the fixed player layout used by the machine-code player:
segments 7–11 for the player and materialized main memory, and segments 16
onward for the raw-file staging area. It therefore needs a contiguous primary
mapper with enough free segments. A 512 KiB primary mapper is recommended for
the included banked test files; 128 KiB is not enough for this fixed layout.

The player supports ordinary KSCC/KSSX files and uncompressed 16K banked data
whose declared bank range does not overlap the player/main/staging ranges. The
sound hardware named by the KSS header must be present; insert SCC or
MoonSound/OPL4 as required by the selected file.

Build or rebuild the COM with:

```sh
python3 tools/build_msx_dos2_kss.py
```

The COM uses MSX-DOS2 function calls 43H/48H/4AH/45H for open/read/seek/close
and the DOS2 extended-BIOS mapper support routines for primary-segment
allocation and page-2 mapping.
