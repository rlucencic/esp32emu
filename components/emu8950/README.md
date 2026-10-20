# emu8950

Mitsutaka Okazaki's [emu8950](https://github.com/digital-sound-antiques/emu8950)
v1.2.2 (2026-10-08), unchanged: the Yamaha Y8950 (MSX-AUDIO), YM3526 (OPL)
and YM3812 (OPL2) in C, MIT licence (`LICENSE`). esp32pc's AdLib and Sound
Blaster use it as the YM3812 (`OPL_setChipType(opl, 2)`).

Its rate converter works in doubles for every sample, which the S3 and the
P4 do in software: run it at the chip's own rate (`clock / 72`, 49,716 Hz
at 3.58 MHz, which turns the converter off) and resample in the machine.

On the boards its allocations go to PSRAM first (`emu8950_alloc.c`: the
CMakeLists compiles the library with `calloc` and `malloc` renamed), and its
tables (~10 KB of .bss) too where the project sets
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` (`linker.lf`): the S3's
internal RAM is short. The source files themselves are as released;
`emuadpcm.h` moved to `include/` (`emu8950.h` includes it).
