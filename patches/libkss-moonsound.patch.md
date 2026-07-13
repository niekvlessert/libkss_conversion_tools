# libkss MoonSound patch

This patch adds the MoonSound/YMF278B and DMV1 MBWave support used by the
conversion tools. It is applied automatically during CMake configuration by
`cmake/apply_libkss_patch.cmake` and is skipped when already applied.

## Header changes

The important API and structure changes are in the two libkss headers:

### `src/kssplay.h`

- Adds `void *moonsound` to `KSSPLAY` for the attached raw MoonSound device.
- Adds MBWave timing state:
  - `mbwave_timing`
  - `mbwave_base_frequency`
  - `mbwave_runtime_ready`
- Adds the public attachment function:

  ```c
  void KSSPLAY_set_moonsound(KSSPLAY *kssplay, void *device);
  ```

### `src/vm/vm.h`

- Adds MoonSound I/O callback state to `VM`:
  - callback context
  - write callback
  - read callback
- Adds:

  ```c
  void VM_set_moonsound(
      VM *vm, void *context,
      void (*write)(void *context, uint32_t a, uint32_t d),
      uint32_t (*read)(void *context, uint32_t a));
  ```

## Implementation changes

- `vm.c` routes the MoonSound port range through the registered callbacks.
- `kssplay.c` connects the device, resets it, mixes its output, and detects
  DMV1 MBWave timing from the MWM header/runtime state.
- No on-disk KSS file-header format is changed by this patch.
