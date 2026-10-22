# Raspberry Pi 5 emulator projects

This workspace is the ESP32 implementation and reference for a separate Raspberry Pi 5 emulator family. Keep the ESP32 projects and their names unchanged.

## Project names and repositories

- The shared Raspberry Pi 5 Linux runtime is `rpi5emu`: <https://github.com/rlucencic/rpi5emu>.
- The ZX Spectrum emulator is `rpi5zx`: <https://github.com/rlucencic/rpi5zx>. It uses `rpi5emu` and is based on the machine emulation in `../esp32zx`.
- Name future Raspberry Pi 5 machine projects with the `rpi5` prefix followed by the machine name, such as `rpi5mac` or `rpi5pc`.
- These Raspberry Pi repositories are separate public GitHub projects. Do not copy ESP-IDF project configuration into them as if it were a Linux build system.

## Platform and porting direction

- Initial target: Raspberry Pi 5, 64-bit ARM Linux; the development machine currently runs Ubuntu 26.04 LTS (aarch64) with SDL 3.4.2.
- `rpi5emu` owns the Linux host runtime shared by the machine-specific `rpi5*` projects. `rpi5zx` owns ZX Spectrum machine integration and must depend on the shared runtime rather than duplicating it.
- Preserve and reuse portable CPU cores, machine hardware emulation, rendering callbacks, disk formats, and debugger protocol where practical. Keep platform-specific startup, display, audio, input, storage, settings, and threading behind the Linux runtime.
- Use SDL 3 for the initial Linux display, audio, keyboard, mouse, and controller backend. Use ordinary Linux files for ROMs, disk images, and settings.
- Treat the existing `emu_machine_t` API as a useful design reference, not as a requirement to carry ESP-IDF dependencies into Linux builds.
- Retain upstream license notices and attribution when porting code from `esp32emu` or its machine projects. Do not silently change the licensing of copied code.

## Initial milestone

Bring up `rpi5zx` on the Pi 5 as the first end-to-end port through `rpi5emu`. Establish a reusable host build and runtime boundary before porting additional machines. The repositories begin as project scaffolds; do not describe a machine as working until it has been built and run on the target.
