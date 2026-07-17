# libkss MoonSound patch

This patch contains the complete local `libkss` integration used by the
conversion tools: MoonSound/YMF278B audio, DMV1 MBWave timing, mapper
handling, and the compressed/expanded complete-page materializers used by
the Quarth and Salamander conversions. It is applied automatically during
CMake configuration by `cmake/apply_libkss_patch.cmake` and is skipped when
already applied.

## Header changes

The important API and structure changes are in the two libkss headers:

### `src/kssplay.h`

- Adds `void *moonsound` to `KSSPLAY` for the attached raw MoonSound device.
- Adds MBWave timing state:
  - `mbwave_timing`
  - `mbwave_base_frequency`
  - `mbwave_runtime_ready`
- Adds mapper state and logical-to-physical bank mapping for relocated DOS2
  allocations and complete-page materialization.
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
- `kssplay.c` connects the device, resets it, mixes its output, detects DMV1
  MBWave timing from the MWM header/runtime state, and materializes the
  SCPX/SCPZ/QCPZ complete-page containers before emulation.
- `vm.c` supports logical-to-physical bank mapping, mapper-window selection,
  loader-specific SCC suppression, and the host-side map request gateway.
- No on-disk KSS file-header format is changed by this patch.
