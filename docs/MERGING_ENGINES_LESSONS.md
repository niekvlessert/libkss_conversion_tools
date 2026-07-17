# Lessons learned: merging compressed engines into one KSS

This is a short handoff for future work on a KSS archive that embeds multiple playback engines and track types.

## KSS constraints

- A KSSX file has one common load address, init address, and play address. Use a small dispatcher for multiple engines.
- The KSS header stores the first/last song numbers, but standard libkss passes the selected song to the Z80 in register `A`. The current playback path therefore reliably supports only song numbers `0..255`.
- KSS bank count is seven bits: at most 127 extra banks. The high bit of byte `0x0D` selects 8K banking; otherwise the file uses 16K banking.
- In 8K mode, bank selection is done by writing to memory address `0x9000` (and sometimes `0xB000`). In 16K mode, libkss handles `OUT (0xFE),A`.
- Header fields used by the merged converter:
  - `0x04/0x06`: load address and initial load size
  - `0x08/0x0A`: common init and play addresses
  - `0x0C/0x0D`: bank offset, mode, and bank count
  - `0x0F`: device flags
  - `0x10`: offset from the end of the extra header to `INFO`
  - `0x18/0x1A`: first and last song

## Engine layout

The merged image uses one contiguous initial load with both engines resident:

- SNG driver: `0x4000`, play routine at `0x4030`
- Pro Tracker player: `0xCA00`, play routine at `0xCAD7`
- Common dispatcher/init/play code: around `0x3000`
- ZX0 decoder: `0xD500`
- SNG decompression buffer: `0x6000..0x7FFF`
- Track metadata tables: high RAM, currently around `0xE000`

The common init receives the global song number in `A`, looks up an engine/descriptor record, then either initializes the SNG driver or decompresses a Pro module and jumps to the Pro player. The common play routine dispatches to the selected engine’s play routine.

## Banking and compression

- Use one bank mode for the complete file. 8K mode is the natural choice because the SNG driver already uses the `0x9000` mapper.
- Do not concatenate compressed data across a bank boundary. Each ZX0 stream must be wholly contained in one mapped 8K bank.
- Pack variable-sized ZX0 streams into the physical 8K banks and store a map entry containing the physical bank and source address.
- SNG data is originally a sequence of raw 8K stream banks. Compress each logical SNG bank independently, then decompress the selected logical bank into the RAM buffer at `0x6000`.
- Pro data can be compressed as one ZX0 stream per module because the current modules fit in one 8K compressed chunk.
- SNG stream jumps originally target `0x9900`, which is the SCC mapper-gap address. When SNG data is read from the RAM buffer at `0x6000`, convert that target to `0x7900`.
- Keep ZX0 optimization bounded. Running the optimizer over a complete 64K reconstructed image, or running multiple algorithms unnecessarily, can make conversion appear to hang.

## Important Z80 pitfalls

- Z80 has no `LD L,(DE)` or `LD H,(DE)` instruction. Load through `A`, for example `LD A,(DE); LD L,A`.
- Replacing an original direct bank write with `CALL loader` can clobber registers that the original code still needs. The SNG loader must preserve `HL` during initialization because `HL` still points into the track table after the bank select.
- The SNG driver’s logical bank numbers are local to each compiled SNG. Do not use the original bank byte as a global archive bank number; use a per-track logical-to-physical map.
- Keep the Pro data destination below the common dispatcher. The current Pro files fit below `0x3000`; otherwise the resident code must move.
- The main image is initialized with `0xC9` fill. This preserves the behavior of the original Pro conversion for data beyond the module’s file length.

## Track selection policy

- Sort inputs deterministically and log every accepted track with its engine type.
- Skip invalid SNG/PRO files with a warning.
- If more than 256 valid tracks exist, remove the smallest valid Pro files by original file size until the archive is selectable through standard libkss. The current repository has 98 valid SNG tracks and 163 valid Pro tracks, so five Pro files are skipped.
- Keep the `INFO` records in exactly the same order as the dispatcher table.

## Verification checklist

1. Build with C99 warnings enabled and run `git diff --check`.
2. Convert one SNG and one Pro file into a two-track test archive.
3. Render both tracks with the same `kss2wav` binary used by the user.
4. Render representative first, middle, last, SNG, and Pro tracks from the full archive.
5. Scan every song for nonzero audio. Use at least 5–10 seconds: some tracks have delayed or very quiet introductions and can look silent during a one-second test.
6. Check the KSS header, bank count, `INFO` count, and first/last song values independently of audio rendering.

`kss2wav` does not accept spaces between an option and its value; use forms such as `-s31`, `-p5`, and `-o/output.wav`.
