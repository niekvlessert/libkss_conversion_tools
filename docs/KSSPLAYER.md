# `kspplayer`

`kspplayer` is a small command-line player for testing generated KSP and KSS archives. It renders through the patched `libkss` emulator and sends 16-bit PCM directly to the default audio device using SDL2; it does not create a WAV file. `kssplayer` is also built as a compatibility name.

For KSP files, the embedded `MWK ` samplepack is loaded automatically. The
MoonSound player automatically looks for `yrw801.rom` in the current directory,
then in the libmoonsound directory used at build time. You can override this
with `--opl4-rom FILE` or the `KSP_YRW801_ROM` environment variable.

SDL2 is the only runtime audio dependency. The same source works on macOS, Linux, and Windows.

## Build

Check out the repository recursively so the upstream libkss emulator modules are present:

```sh
git clone --recurse-submodules <repository-url>
cmake -S . -B build
cmake --build build --target kspplayer
```

On macOS with Homebrew, install SDL2 if needed:

```sh
brew install sdl2
```

## Use

Play song 99 from a merged archive for a short test:

```sh
./build/kspplayer --song 0 --seconds 10 /path/to/song.ksp

# Or select a ROM explicitly:
./build/kspplayer --song 0 --seconds 10 \
  --opl4-rom /path/to/yrw801.rom /path/to/song.ksp
```

Useful options include `--mwk FILE` to override the embedded samplepack,
`--channels 2`, `--quality 0`, `--loops 0` to disable loop-triggered fading,
and `--info` to inspect the KSP/KSS metadata without opening audio.

For banked KSS testing, `--mapper-base N` places logical bank 0 at physical
mapper segment N. Subsequent logical banks use subsequent physical segments.
This exercises the same logical-to-physical mapping contract used by the MSX
player; the engine must request a logical bank and must not choose the
physical segment itself. For example:

    SDL_AUDIODRIVER=dummy ./build/kssplayer --mapper-base 9 \
      --song 0 --seconds 10 /path/to/quarth_16k.kss

`quarth_16k_complete_page.kss` is also supported. Its QCPX materializer
builds each complete engine+music page once and maps the selected logical page
into the `4000H..7FFFH` window, so `--mapper-base` still selects the physical
segment range without requiring duplicate engine bytes in the file.

While playing, press Escape or Ctrl-C to stop early. The player restores the terminal mode before returning to the shell.
