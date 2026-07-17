# KSP / KSS Plus MoonSound Implementation Plan

## 1. Goal

Create a minimally extended KSS-derived format named **KSP (KSS Plus)** that can package and play MoonSound/OPL4 music while preserving the existing KSS execution model as much as possible.

The first supported content type will be:

- one MoonBlaster for MoonSound WAVE replayer engine;
- one `.MBW` song;
- the small KSS-compatible bootstrap needed to start the engine;
- enough metadata to load and play the file on both a desktop renderer and a real MSX.

Required deliverables:

1. A written KSP format specification.
2. A host-side KSP reader/writer library.
3. MoonSound/YMF278B emulation through libvgm.
4. `ksp2wav`, a command-line KSP-to-WAV renderer.
5. `kspack`, a command-line packer that combines an MBWave engine and one `.MBW` file into one `.ksp` file.
6. `KSPPLAY.COM`, an MSX-DOS player that loads `.ksp` files from disk and plays them on a real MoonSound.
7. Validation, tracing, and regression-test tools.

The user will supply all third-party binaries, sources, ROMs, and music files. They must not be committed to this project.

---

## 2. Naming decision

Use **KSP** consistently throughout the project:

- **Format name:** KSP
- **File extension:** `.ksp`
- **Meaning:** KSS Plus
- **Desktop renderer:** `ksp2wav`
- **Archive builder:** `kspack`
- **MSX player:** `KSPPLAY.COM`

Use only these KSP names in code, documentation, commands, and generated files.

---

## 3. Design principles

### 3.1 Preserve KSS

A KSP file begins with the ordinary 32-byte `KSSX` header. The load image is
stored once in the KSP chunks and materialized by a KSP-aware player; it is
not duplicated between the header and directory.

Do not:

- replace the `KSSX` magic;
- reinterpret existing KSS fields;
- use bits that the KSSX specification marks as reserved;
- increase the fixed KSS header size in a way that breaks existing parsers;
- require old KSS players to understand OPL4.

KSP-specific data is appended immediately after the KSSX header.

### 3.2 Graceful behavior in old players

A `.ksp` file is identified by its KSSX header and KSP trailer. A KSS-only
player cannot materialize the compact load image, so KSP-aware entry points
must handle the file before calling libkss. Old KSS players are not a playback
target for the compact layout:

- no crash;
- no infinite loop;
- no invalid mapper activity;
- no uncontrolled writes to unsupported ports;
- silence is acceptable.

The bootstrap should require a small **KSP capability block** prepared by a KSP-aware host or by `KSPPLAY.COM`. If the signature is absent, its INIT routine returns immediately and PLAY becomes a no-op. This is more deterministic than relying only on a MoonSound hardware probe.

### 3.3 Standard MoonSound ROM is hardware

The standard YRW801 instrument ROM is treated as part of the target MoonSound hardware.

KSP must not:

- embed the standard YRW801 ROM;
- declare an external YRW801 file dependency;
- store a path or filename for that ROM.

The desktop test player may obtain the ROM through the local libvgm integration if the selected emulator core requires it, but that is a local runtime configuration issue and not part of the KSP format.

### 3.4 Load only what is needed

Although version 1 packages one engine and one song, the container must use a directory so later versions can contain:

- multiple songs;
- multiple engines;
- optional sample sets;
- metadata;
- artwork;
- compressed chunks.

A player must be able to seek directly to the required engine and song without decompressing the whole file.

### 3.5 Separate format, host emulation, and MSX runtime

Keep these layers independent:

1. KSP parser/writer.
2. KSS/Z80 execution host.
3. OPL4 device backend.
4. MBWave engine adapter.
5. Desktop WAV renderer.
6. Real-MSX disk player.

This prevents MBWave-specific assumptions from becoming permanent format restrictions.

---

## 4. Proposed KSP file layout

```text
+-------------------------------+ 0
| Standard KSSX header (32 B)   |
+-------------------------------+ 0x20
| KSP chunks                    |
|   ENGN: engine binary         |
|   SONG: one MBW file          |
|   EDES: engine descriptor     |
|   META: optional metadata     |
|   ...future chunks...         |
+-------------------------------+
| KSP directory                 |
+-------------------------------+
| Fixed KSP trailer             | physical EOF
+-------------------------------+
```

### 4.1 KSSX header

The 32-byte KSSX header remains unchanged and describes the image that a
KSP-aware player will build from `ENGN`, `SONG`, and `EDES`. The bytes after
the header are KSP chunks, not a KSS load image. A materializer must place the
engine and song at the descriptor addresses and install any engine-specific
bootstrap before calling libkss.

### 4.2 KSP trailer

Use a fixed-size trailer at physical EOF so a KSP player can find the directory with one seek from the end of the file.

Provisional version-1 trailer:

```c
struct KspTrailerV1 {
    char     magic[4];          // "KSP1"
    uint16_t trailer_size;      // sizeof(KspTrailerV1)
    uint16_t format_minor;      // initially 0
    uint32_t directory_offset;  // absolute file offset
    uint32_t directory_size;
    uint32_t directory_crc32;
    uint32_t flags;             // must be 0 in version 1
};
```

All integers are little-endian.

The exact structure should be frozen only after parsers have been prototyped on:

- macOS;
- Linux;
- Windows;
- Z80/MSX-DOS.

### 4.3 Directory header

```c
struct KspDirectoryHeaderV1 {
    char     magic[4];          // "KDIR"
    uint16_t header_size;
    uint16_t entry_size;
    uint32_t entry_count;
    uint32_t flags;
};
```

Use 32-bit offsets and lengths from the first version. This avoids the original KSS 64 KiB and limited-bank-count restrictions for appended assets.

### 4.4 Directory entry

Provisional fixed-size entry:

```c
struct KspDirectoryEntryV1 {
    char     type[4];           // FourCC
    uint32_t id;                // unique within this type
    uint32_t offset;            // absolute file offset
    uint32_t packed_size;
    uint32_t unpacked_size;
    uint32_t crc32;             // unpacked data
    uint16_t compression;       // 0 = none
    uint16_t flags;
    uint32_t aux;               // type-specific value or 0
};
```

Version 1 initially supports only uncompressed chunks. The fields for packed size and compression are included now so compression can be added without redesigning the directory.

Unknown chunk types must be ignored unless an entry is marked required.

### 4.5 Initial chunk types

| FourCC | Purpose | Required |
|---|---|---:|
| `ENGN` | Z80 MBWave replayer engine binary | Yes |
| `SONG` | One original `.MBW` file | Yes |
| `EDES` | Engine load addresses, entry points, RAM requirements, and capabilities | Yes |
| `META` | UTF-8 title/author/comment metadata | No |
| `BOOT` | Optional explicit copy of the KSS bootstrap | No |
| `MWV ` | Future MoonBlaster sample/instrument data | No |
| `COVR` | Future artwork | No |

Do not require `MWV ` in the first milestone. Initial test songs should use only the standard MoonSound ROM tones or otherwise be fully supported by the supplied MBWave engine and song combination.

---

## 5. Engine descriptor and ABI

The original MBWave replayer may not expose a stable generic interface. Add a thin Z80 adapter around the supplied engine rather than teaching every host about MBWave internals.

### 5.1 Engine descriptor

The `EDES` chunk should contain a small binary descriptor:

```c
struct KspEngineDescriptorV1 {
    char     magic[4];             // "KED1"
    uint16_t descriptor_size;
    uint16_t engine_type;          // 1 = MBWave
    uint16_t load_address;
    uint16_t init_address;
    uint16_t play_address;
    uint16_t stop_address;         // 0 if unavailable
    uint16_t capability_address;
    uint16_t song_window_address;
    uint16_t work_address;
    uint16_t work_size;
    uint16_t tick_rate_num;        // normally 50 or 60
    uint16_t tick_rate_den;        // normally 1
    uint32_t minimum_mapper_ram;
    uint32_t flags;
};
```

Addresses and exact calling conventions must be based on the supplied MBWave engine, not guessed.

### 5.2 KSP capability block

Before INIT, the KSP-aware host writes a small capability structure to the descriptor’s `capability_address`.

It should include:

- signature, for example `KSP1`;
- structure version;
- MoonSound-present flag;
- PAL/NTSC mode;
- song data location or mapper handle;
- song length;
- optional host-service entry point.

A legacy KSS host will not create this block. The bootstrap detects that and exits silently.

### 5.3 Initial calling convention

Prefer a KSS-like interface:

- INIT: called once with the selected song number in registers compatible with the existing KSS host;
- PLAY: called at the declared tick rate;
- STOP: called before unloading;
- no disk access from interrupt context;
- no dependency on MSX BIOS calls during normal PLAY.

Where possible, the adapter should convert the KSS INIT/PLAY calls into the original MBWave engine’s expected calls.

### 5.4 Engine investigation task

Before implementing the adapter:

1. Identify the actual replayer code and required tables in the supplied MBWave files.
2. Determine load address, relocation behavior, work RAM, and stack requirements.
3. Determine how an `.MBW` file is passed to the replayer.
4. Determine whether the song must be contiguous in the Z80 address space.
5. Identify all OPL4 I/O ports used.
6. Determine tick frequency and whether timing differs between PAL and NTSC.
7. Determine whether the engine performs disk I/O itself.
8. Determine dependencies on `.MWV`, `.MWK`, `.MWM`, configuration files, or other resources.
9. Capture a known-good register trace from openMSX or real hardware.
10. Document the discovered engine ABI before writing the final packer.

Deliverable: `docs/mbwave-engine-abi.md`.

Use the supplied Dutch MoonSound Veterans material as the primary reference
set for this investigation:

- `DMV1.dsk`, `DMV2.dsk`, `DMV3.dsk`, and `DMVFT.dsk` are runnable reference
  media for openMSX;
- the matching ZIP files are convenient sources for individual `.MWM`, `.MWK`,
  and `.BIN` files without first writing a disk-image extractor;
- the `.MWM` files are song candidates, `.MWK` files are associated resource
  candidates, and the `WAVEDRVR.BIN`/`DISKCHR*.BIN`/`TITEL*.BIN` files are
  player or disk-support candidates until disassembly proves otherwise.

The local reverse-engineering loop is:

1. Extract one candidate player/resource set from a ZIP.
2. Disassemble binaries with `z80dasm`, using an explicit origin and a symbol
   file whenever the load address is known.
3. Run the original disk in openMSX with `openmsx_debug/run_openmsx_trace.sh`.
4. Narrow the PC and memory ranges around the player, and enable the optional
   I/O trace for the suspected MoonSound ports.
5. Compare the openMSX trace with the disassembly and document the load/init/
   play ABI before writing an adapter.

For every ZX0 experiment, use the installed `dzx0` executable as an
independent decompression oracle. A stream is not considered valid until
`dzx0` reproduces the original bytes and the result matches a host-side CRC.

---

## 6. Repository layout

```text
ksp/
├── CMakeLists.txt
├── plan.md
├── README.md
├── docs/
│   ├── ksp-format.md
│   ├── mbwave-engine-abi.md
│   ├── msx-player-memory-map.md
│   └── third-party-setup.md
├── include/
│   └── ksp/
│       ├── format.h
│       ├── reader.h
│       ├── writer.h
│       ├── engine.h
│       └── opl4_device.h
├── src/
│   ├── format/
│   ├── player/
│   ├── opl4/
│   └── adapters/
│       └── mbwave/
├── tools/
│   ├── ksp2wav/
│   ├── kspack/
│   ├── kspinfo/
│   └── kspvalidate/
├── msx/
│   ├── kspplay/
│   ├── bootstrap/
│   └── common/
├── tests/
│   ├── public/
│   ├── synthetic/
│   └── private/          # gitignored
├── third_party/
│   ├── README.md
│   ├── libkss/           # user supplied or CMake path
│   ├── libvgm/           # user supplied or CMake path
│   └── mbwave/           # user supplied, always gitignored
└── cmake/
```

Do not copy third-party code directly into project source files. Keep modifications as small patch sets or adapter layers wherever practical.

---

## 7. Host-side KSP library

Implement a small standalone library, tentatively `libksp`, before modifying libkss.

### 7.1 Reader responsibilities

- validate the initial KSSX image;
- locate and validate the physical KSP trailer;
- read the directory;
- reject integer overflows and overlapping invalid chunks;
- expose chunks by type and ID;
- verify CRC32 when requested;
- load one chunk without loading the entire file;
- work with memory buffers and seekable files;
- preserve unknown chunks during rewrite when possible.

### 7.2 Writer responsibilities

- accept a valid KSS-compatible bootstrap image;
- append engine, song, descriptor, and metadata chunks;
- align chunks only where useful;
- generate directory and trailer;
- calculate CRC32 values;
- write atomically through a temporary file;
- optionally emit a map file showing offsets and sizes.

### 7.3 Validation rules

Reject a file when:

- the KSSX prefix is invalid;
- the KSP trailer points outside the physical file;
- directory multiplication or offset arithmetic overflows;
- chunks overlap the directory or trailer illegally;
- required chunks are missing;
- an `EDES` address range is impossible;
- unpacked size is inconsistent for uncompressed data;
- duplicate required IDs create ambiguity;
- a required compression method is unsupported.

---

## 8. libkss integration

Use libkss for the existing KSS/Z80 execution model, but keep KSP parsing outside libkss initially.

### 8.1 First integration approach

1. Parse the `.ksp` file with `libksp`.
2. Load the `ENGN`, `SONG`, and `EDES` chunks.
3. Materialize the KSS load image from the header and descriptor.
4. Place the engine and song in the emulated MSX memory according to the descriptor.
5. Write the KSP capability block.
6. Register MoonSound I/O handlers.
7. Call the ordinary KSS INIT and PLAY paths.

This minimizes invasive changes to libkss.

### 8.2 Changes likely needed in libkss

- public or internal hooks for Z80 memory writes before INIT;
- access to mapper RAM or an API for allocating mapper pages;
- I/O read and write handlers for MoonSound;
- stereo audio device output;
- reset and clock lifecycle hooks;
- optional register-write tracing;
- a way to mix the OPL4 output with existing KSS devices;
- ability to expose runtime errors rather than silently continuing.

Prefer a generic external sound-device interface rather than hard-coding MBWave into libkss.

### 8.3 OPL4 port mapping

Do not freeze the MoonSound I/O map from memory. Verify all ports and status-register behavior against:

- MoonSound/YMF278B documentation supplied by the user;
- a known emulator implementation;
- a trace from the original MBWave player.

Implement both reads and writes. Some engines may poll status flags and will fail if reads always return `0xFF`.

---

## 9. libvgm OPL4 backend

Create an adapter around libvgm’s YMF278B implementation.

### 9.1 Adapter interface

```cpp
class Opl4Device {
public:
    bool initialize(uint32_t clock_hz, uint32_t sample_rate);
    void reset();

    uint8_t read(uint16_t io_port);
    void write(uint16_t io_port, uint8_t value);

    void render(int16_t* interleaved_stereo, size_t frames);
    void set_trace_sink(Opl4TraceSink* sink);
};
```

The actual libvgm API may differ; isolate it inside one translation unit.

### 9.2 Timing model

The Z80/KSS host and OPL4 renderer must advance from the same clock/timestamp source.

For each PLAY tick:

1. run the Z80 INIT/PLAY code;
2. timestamp all OPL4 writes;
3. render the exact number of audio frames until the next tick;
4. carry fractional samples between ticks;
5. mix with any other enabled KSS sound devices.

Do not emit all register writes at the start of an audio block without timing if the engine performs meaningful delays or status polling inside a tick.

### 9.3 ROM handling

The KSP format does not contain the standard MoonSound ROM.

For local desktop testing:

- use the libvgm-supported method for obtaining or configuring the ROM;
- place the user-supplied file outside the repository;
- allow a command-line or environment-variable path if needed;
- never record that path in the `.ksp` file;
- produce a clear diagnostic when the selected libvgm core cannot run without the local ROM.

### 9.4 Trace support

Add a trace mode containing:

- emulated timestamp;
- Z80 PC;
- I/O port;
- register index where applicable;
- written value;
- status reads;
- current song tick.

This is essential for comparing `ksp2wav` with openMSX or the original MBWave player.

The repository's `openmsx_debug` helper is the reference trace setup for this
work. Keep its trace output text-based and diffable. It must support, at
minimum:

- PC plus optional disassembly;
- writes to selected RAM ranges;
- MSX RAM-mapper writes;
- cartridge mapper writes;
- configurable I/O write ranges for MoonSound probing;
- the PC responsible for each write.

Do not assume the MoonSound I/O range from memory. Start with a configurable
range, confirm it from the original player and openMSX device configuration,
then record the confirmed range in `docs/mbwave-engine-abi.md`. The first
disassembly of the supplied `WAVEDRVR.BIN` shows `C4H-C7H` and `7EH-7FH`; both
must be traced before deciding which accesses belong to MoonSound and which
belong to another device.

---

## 10. `ksp2wav`

`ksp2wav` is the primary desktop verification tool. It accepts `.ksp` files despite its requested historical name.

### 10.1 Initial command line

```text
ksp2wav [options] input.ksp output.wav
```

Initial options:

```text
--length SECONDS       hard render limit
--loops COUNT          number of loops, default 2 when loop detection exists
--fade SECONDS         fade duration
--rate HZ              output rate, default 44100
--pal                   force 50 Hz
--ntsc                  force 60 Hz
--subsong N             reserved for later multi-song KSP files
--rom PATH              local OPL4 ROM path when required by the backend
--trace FILE            write OPL4/Z80 trace
--info                  show parsed KSP and engine information
--validate              validate without rendering
--no-crc                skip chunk CRC checks
```

### 10.2 WAV output

Initial output:

- PCM;
- signed 16-bit;
- stereo;
- little-endian;
- 44.1 kHz by default.

Later add 24-bit or float output only if useful for debugging.

### 10.3 End detection

Version 1 may use an explicit duration because MBWave end/loop behavior is not yet known.

Implement in this order:

1. hard duration;
2. engine-reported stop flag, if one exists;
3. repeated-state or loop-point detection;
4. silence detection only as an optional fallback.

Never make silence detection the only termination mechanism.

### 10.4 Diagnostics

On failure, print:

- parser stage;
- missing chunk;
- engine descriptor values;
- failing address or offset;
- unsupported compression;
- missing local ROM;
- Z80 execution timeout;
- last OPL4 writes.

---

## 11. `kspack`

### 11.1 Initial command line

```text
kspack mbwave \
  --bootstrap ksp_mbwave_bootstrap.bin \
  --engine mbwave_engine.bin \
  --engine-desc mbwave_engine.json \
  --song example.mbw \
  --output example.ksp
```

Optional metadata:

```text
--title TEXT
--author TEXT
--game TEXT
--comment TEXT
--pal | --ntsc
--map output.map
```

### 11.2 Packer workflow

1. Validate the supplied engine descriptor.
2. Validate the `.MBW` input using known minimum structural checks.
3. Ensure engine, work RAM, stack, bootstrap, and song windows do not overlap.
4. Build the KSSX header and descriptor.
5. Add `ENGN`, `SONG`, `EDES`, and optional `META` chunks.
6. Generate the directory and fixed trailer.
7. Reopen the output and validate it independently.
8. Optionally run a short smoke render through `ksp2wav`.

### 11.3 Engine descriptor source

Use a human-editable JSON or TOML file as packer input, but store a compact binary `EDES` chunk in the KSP.

Example source descriptor:

```json
{
  "engine_type": "mbwave",
  "load_address": "0x8000",
  "init_address": "0x8120",
  "play_address": "0x8150",
  "stop_address": "0x0000",
  "capability_address": "0xF000",
  "song_window_address": "0x4000",
  "work_address": "0xC000",
  "work_size": 8192,
  "tick_rate": "50/1",
  "minimum_mapper_ram": 131072
}
```

These addresses are examples only and must not be used until verified.

### 11.4 No engine redistribution

`kspack` may package a user-supplied engine into a local KSP file, but the repository and public test artifacts must not redistribute the MBWave engine unless its license explicitly permits that.

---

## 12. MSX player: `KSPPLAY.COM`

### 12.1 Initial target

Target:

- MSX2 or newer;
- MSX-DOS2 or Nextor;
- MoonSound/OPL4;
- at least 128 KiB mapper RAM;
- disk file with seek support.

A DOS1 build can be considered later. Do not let DOS1 constraints weaken the initial format.

### 12.2 Runtime workflow

1. Open the `.ksp` file.
2. Seek to physical EOF and read the KSP trailer.
3. Seek to and read the directory.
4. Locate `ENGN`, `SONG`, and `EDES`.
5. Check MoonSound presence.
6. Allocate mapper segments.
7. Load the engine and current song only.
8. Build the KSP capability block.
9. Install the playback interrupt.
10. Call engine INIT.
11. Call PLAY at the declared frequency.
12. Handle keyboard commands in the foreground.
13. On exit, call STOP, silence OPL4, restore the interrupt, close the file, and free mapper segments.

### 12.3 Disk strategy

Do all disk reads outside the playback interrupt.

For the first one-song implementation:

- read the small directory into conventional RAM;
- load the engine once;
- load the selected MBW into mapper RAM;
- keep a small conventional-RAM window visible to the engine;
- switch mapper segments only in controlled adapter code.

If the original MBWave engine requires a contiguous song buffer smaller than 64 KiB, map or copy it into a fixed window before INIT.

If it requires more than one visible segment, add an adapter callback or patched song reader. Do not perform DOS calls from PLAY.

### 12.4 Memory-map document

Create `docs/msx-player-memory-map.md` with:

- DOS/TPA usage;
- code and stack;
- directory buffer;
- engine location;
- work RAM;
- capability block;
- visible song window;
- mapper segment ownership;
- interrupt handler;
- reserved BIOS/DOS areas.

Test on a 128 KiB MSX2 configuration first, because that is the minimum practical target.

### 12.5 User interface

Keep version 1 simple:

```text
KSPPLAY filename.ksp
```

Controls:

- Space: pause/resume
- Esc: stop and exit
- Left/Right: reserved for previous/next song
- `I`: show file and engine information

No graphical UI is required.

### 12.6 Error recovery

Every exit path must restore:

- interrupt vectors;
- mapper state;
- stack;
- OPL4 silence;
- DOS file handles.

A corrupt KSP file must fail before installing the playback interrupt.

---

## 13. Compression

Do not implement compression in the first playable milestone.

The directory supports it, but version 1 tools initially accept:

```text
compression = 0  // none
```

After uncompressed playback is correct, evaluate:

- ZX0 for MSX-side decompression;
- chunk-local compression only;
- engine and song compressed independently;
- bounded scratch-memory requirements;
- decompression before playback, never in the audio interrupt;
- CRC over uncompressed data.

For any ZX0 implementation, validate each stream in two independent ways:

1. decompress the exact stream with the installed `dzx0` command-line tool;
2. run the target Z80 decoder and compare the complete output byte-for-byte.

Use `z80dasm` on the generated target decoder and keep a small host-side
round-trip fixture for each decoder mode. This prevents a compressor or host
model from appearing correct while the MSX decoder is corrupting data.

The player must never need to decompress unrelated songs or artwork.

---

## 14. Testing strategy

### 14.1 Public synthetic tests

Create distributable synthetic fixtures that do not contain third-party code:

- minimal KSSX header;
- minimal KSP directory and trailer;
- malformed offsets;
- overlapping chunks;
- bad CRC;
- unknown optional chunk;
- unknown required chunk;
- synthetic Z80 engine that writes a small deterministic OPL4 register sequence.

### 14.2 Private MBWave tests

Place user-supplied fixtures in `tests/private/`, excluded by `.gitignore`.

Maintain a local manifest containing:

- filename;
- SHA-256;
- expected tick rate;
- expected duration;
- expected first register writes;
- expected audio checksum after rendering;
- notes about external assets.

The manifest should also record the source disk/ZIP, the player binary used,
the disassembler origin, and the openMSX trace filename. Keep these references
local and gitignored when they contain supplied third-party material.

### 14.3 Cross-player comparison

For one reference `.MBW`:

1. Play through the original MBWave environment in openMSX.
2. Record OPL4 writes with timestamps.
3. Render the packed `.ksp` through `ksp2wav`.
4. Compare register sequences.
5. Compare generated audio after accounting for emulator-core differences.
6. Test `KSPPLAY.COM` in openMSX.
7. Test on real MoonSound hardware if available.

Register traces are the primary correctness test. Bit-identical audio is desirable but may not be realistic across different OPL4 cores.

### 14.4 Compatibility tests

- Verify ordinary `.kss` files still render unchanged.
- Verify KSP parsing does not affect non-KSP libkss paths.
- Verify a KSP file presented to an old KSS host exits safely or remains silent.
- Verify `.ksp` physical sizes above 2 MiB.
- Verify directory entry counts well above 255, even before multi-song playback is implemented.
- Verify all 32-bit offset calculations near their limits.

### 14.5 MSX robustness tests

Test under openMSX with:

- MSX2 and exactly 128 KiB RAM;
- PAL and NTSC;
- MSX-DOS2;
- Nextor;
- slow disk image;
- fragmented or deliberately slow host storage;
- missing MoonSound;
- truncated file;
- invalid CRC;
- Escape during loading and playback.

### 14.6 Reverse-engineering and decompression tools

The minimum local tool set is now available:

- `z80dasm` for repeatable Z80 disassembly;
- `dzx0` for independent ZX0 decompression checks;
- openMSX plus `openmsx_debug` for execution, memory, mapper, and I/O traces;
- the Dutch MoonSound Veterans disk images and ZIP archives as reference
  programs and music data.

When a trace is ambiguous, the next useful artifact is a short trace of one
known-good song from the original player, including the first engine INIT and
several PLAY ticks. No additional general-purpose tool is required before
Phase 0 can begin.

---

## 15. Build system

Use CMake for desktop components.

Suggested options:

```text
KSP_BUILD_TOOLS=ON
KSP_BUILD_TESTS=ON
KSP_USE_SYSTEM_LIBKSS=OFF
KSP_USE_SYSTEM_LIBVGM=OFF
KSP_ENABLE_TRACE=ON
KSP_ENABLE_PRIVATE_TESTS=OFF
```

Allow third-party locations through cache variables:

```text
LIBKSS_ROOT=/path/to/libkss
LIBVGM_ROOT=/path/to/libvgm
MBWAVE_PRIVATE_ROOT=/path/to/private/files
OPL4_ROM_PATH=/path/to/local/rom
```

For MSX:

- use the assembler/compiler selected after inspecting the engine and bootstrap;
- keep the Z80 adapter and critical interrupt path in assembly;
- a small C layer is acceptable for DOS file handling if its memory cost is controlled;
- provide a reproducible Makefile;
- generate a `.COM` and a map file.

Do not make the desktop build depend on the MSX toolchain.

---

## 16. Implementation phases

### Phase 0 — Input inventory and engine research

Deliverables:

- `docs/third-party-setup.md`;
- `docs/mbwave-engine-abi.md`;
- known-good MBW fixture manifest;
- openMSX OPL4 reference trace;
- confirmed MoonSound I/O map;
- confirmed engine memory requirements.

Use the supplied DMV disk/ZIP pairs, `z80dasm`, `dzx0`, and the openMSX trace
helper during this phase. Establish one complete reference path first: source
song/resource files, player load address, INIT/PLAY entry points, mapper and
MoonSound I/O writes, and a reproducible trace.

Exit criterion: a manually loaded engine and MBW file can be started in a controlled MSX/openMSX experiment.

### Phase 1 — KSP format library

Deliverables:

- `docs/ksp-format.md`;
- `libksp` reader and writer;
- `kspinfo`;
- `kspvalidate`;
- synthetic parser tests.

Exit criterion: KSP files can be created, inspected, round-tripped, and rejected safely when corrupt.

### Phase 2 — MBWave bootstrap and packer

Deliverables:

- KSS-compatible bootstrap;
- binary engine descriptor;
- `kspack mbwave`;
- map-file generation;
- one private `.ksp` fixture.

Exit criterion: the produced file has a valid KSSX header, compact KSP trailer/directory, and contains the supplied engine and MBW exactly once.

### Phase 3 — Host MoonSound integration

Deliverables:

- libvgm OPL4 adapter;
- libkss external-device hooks;
- capability-block injection;
- OPL4 register trace;
- deterministic synthetic OPL4 test.

Exit criterion: a synthetic KSS/KSP engine can write OPL4 registers and produce stereo audio.

### Phase 4 — `ksp2wav`

Deliverables:

- WAV writer;
- KSP loading;
- MBWave INIT/PLAY execution;
- duration/fade controls;
- tracing and diagnostics.

Exit criterion: the reference MBW renders correctly and its OPL4 trace matches the openMSX reference closely enough to explain every difference.

### Phase 5 — MSX disk player

Deliverables:

- `KSPPLAY.COM`;
- directory reader;
- mapper allocator;
- engine/song loader;
- interrupt-driven playback;
- clean exit and error recovery.

Exit criterion: the same KSP file plays in openMSX without conversion or extracted side files.

### Phase 6 — Real-hardware validation

Deliverables:

- real MoonSound test notes;
- PAL/NTSC validation;
- timing corrections;
- RAM-minimum confirmation;
- compatibility matrix.

Exit criterion: at least one KSP file plays correctly on real MSX hardware or, when hardware is unavailable, across two independent MSX emulators.

### Phase 7 — Optional extensions

Only after version 1 is stable:

- multiple songs;
- multiple engines;
- per-chunk ZX0 compression;
- `.MWV` assets;
- UTF-8 multilingual metadata;
- artwork;
- playlists;
- engine switching;
- large collections with thousands of directory entries.

---

## 17. Main technical risks

### MBWave engine is not relocatable

Mitigation:

- preserve its original load address;
- build the MSX memory map around it;
- add a small fixed-address adapter;
- patch only through a documented local transformation step.

### Engine expects DOS or BIOS services

Mitigation:

- identify calls in Phase 0;
- replace disk loading with memory callbacks;
- provide a very small compatibility shim where unavoidable;
- keep all DOS calls outside PLAY.

### Song data does not fit in one visible Z80 window

Mitigation:

- store it in mapper RAM;
- expose a fixed window;
- patch or wrap the engine’s data access;
- record mapper requirements in `EDES`.

### OPL4 status polling differs between emulators

Mitigation:

- implement reads, busy timing, and reset behavior;
- use traces from openMSX/original software;
- isolate core-specific behavior in the libvgm adapter.

### Legacy KSS parser rejects trailing data

Mitigation:

- test before freezing the format;
- keep `.ksp` as a distinct extension;
- provide `kspmaterialize` for diagnostics that need a full KSS load image;
- provide `kspack --legacy-prefix-output file.kss` for diagnostics if useful.

### Third-party licensing is unclear

Mitigation:

- never commit or distribute the MBWave engine;
- require local paths;
- use synthetic CI fixtures;
- keep engine extraction and packaging user-local.

### Audio timing drifts

Mitigation:

- use rational tick-to-sample accumulation;
- timestamp writes;
- compare long renders;
- never round every 50/60 Hz frame independently.

---

## 18. Definition of done for version 1

Version 1 is complete when all of the following are true:

- `.ksp` is the only new file extension.
- Every KSP begins with a KSSX header and materializes its load image from chunks.
- No KSS reserved bits are repurposed.
- The KSP directory is located from a fixed EOF trailer.
- The file contains one `ENGN`, one `SONG`, and one `EDES`.
- `kspack` packages a user-supplied MBWave engine and one MBW file.
- `kspvalidate` detects corrupt offsets, sizes, missing chunks, and bad CRCs.
- `ksp2wav` renders the KSP through libkss plus libvgm OPL4.
- `ksp2wav` can emit a useful OPL4/Z80 trace.
- `KSPPLAY.COM` loads the same KSP directly from disk.
- The MSX player performs no disk I/O in its playback interrupt.
- The standard MoonSound ROM is not embedded or referenced by the KSP format.
- A KSP file is rejected clearly by a KSS-only implementation rather than being treated as a playable legacy KSS.
- Ordinary KSS playback remains unaffected.
- Third-party files are absent from the repository and public CI artifacts.
- One reference MBW has been validated against the original player/openMSX.
- The format document is sufficient for an independent parser implementation.

---

## 19. Recommended first implementation slice

Build the smallest end-to-end path before adding metadata or compression:

1. Inspect the supplied MBWave engine and establish its load/init/play interface.
2. Write a fixed-address KSS bootstrap that checks for the KSP capability signature.
3. Define only `ENGN`, `SONG`, and `EDES`.
4. Implement uncompressed KSP reader/writer and `kspack`.
5. Add libvgm OPL4 I/O to the desktop KSS host.
6. Render exactly 30 seconds with `ksp2wav`.
7. Match the OPL4 trace against openMSX.
8. Implement `KSPPLAY.COM` with the same engine descriptor.
9. Confirm playback from a disk image on a 128 KiB MSX2 configuration.
10. Freeze KSP version 1 only after both desktop and MSX playback use the same file successfully.
