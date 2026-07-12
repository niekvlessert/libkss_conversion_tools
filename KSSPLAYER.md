# `kssplayer`

`kssplayer` is a small command-line player for testing the generated KSS archives. It renders through the upstream `libkss` emulator and sends 16-bit PCM directly to the default audio device using SDL2; it does not create a WAV file.

SDL2 is the only runtime audio dependency. The same source works on macOS, Linux, and Windows.

## Build

Check out the repository recursively so the upstream libkss emulator modules are present:

```sh
git clone --recurse-submodules <repository-url>
cmake -S . -B build
cmake --build build --target kssplayer
```

On macOS with Homebrew, install SDL2 if needed:

```sh
brew install sdl2
```

## Use

Play song 99 from a merged archive for a short test:

```sh
./build/kssplayer --song 99 --seconds 10 /path/to/archive.kss
```

Useful options include `--channels 2`, `--quality 0`, `--loops 0` to disable loop-triggered fading, and `--info` to inspect the KSS header without opening audio.

While playing, press Escape or Ctrl-C to stop early. The player restores the terminal mode before returning to the shell.
