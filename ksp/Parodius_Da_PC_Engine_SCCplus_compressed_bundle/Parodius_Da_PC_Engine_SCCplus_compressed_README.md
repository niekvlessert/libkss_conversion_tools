# Parodius Da! SCC+ compressed KSP

This is the ZX0-compressed `KCPZ` counterpart of `Parodius_Da_PC_Engine_SCCplus_prototype.ksp`.

- KCPZ version: 1
- Public tracks: 38
- Internal pages: 38
- Raw KCPX size: 208,387 bytes
- Compressed KCPZ size: 68,348 bytes
- Reduction: 67.20%
- SHA-256: `c4f123c0054b4b4ea7c54df2995236e019c151406624f636e8ce03480064144d`

The common 16 KB template and every sparse page overlay are independently compressed with standard forward ZX0 v2. Every stream was decompressed again and compared byte-for-byte with the KCPX source. Every reconstructed 16 KB page is byte-identical to the uncompressed container.

The normal current KCPZ materializer is sufficient for song changes. The included SCC+ silence patch remains applicable.
