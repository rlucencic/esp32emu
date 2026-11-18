# esp32emu: the shared board runtime for the emulators

ESP-IDF components used by `../esp32ste` (Atari STE), `../esp32mac`
(Macintosh Plus/SE/Classic), `../esp32cpc` (Amstrad CPC and Plus),
`../esp32zx` (ZX Spectrum 128K, +2, +3), `../esp32cbm` (Commodore 64 and
128), `../esp32xl` (Atari 800XL and 130XE), `../esp32msx` (MSX1, MSX2),
`../esp32gs` (Apple IIGS), `../esp32coco` (Tandy Color Computer 2 and 3,
a 6809 or 6309), `../esp32pc` (IBM PC/XT, an 8088) and `../esp32ql` (Sinclair QL,
a 68008; since 2026-11-18, in development) on two boards: the Makerfabs
MaTouch ESP32-S3 7" (target esp32s3, the default) and the M5Stack Tab5
(target esp32p4); and `../esp32mac2` (Macintosh LC and II, a 68020 and
the II's 68881), `../esp32mac3` (Macintosh LC II and LC III, a 68030
with its MMU), `../esp32arc` (Acorn Archimedes, an ARM2) and `../esp32ami`
(Amiga 500, 2000, 600 and 1200, a 68000 or a 68EC020), the Tab5 only. It is a library: it isn't built on its own. Build and
test through one of the projects (each has `EXTRA_COMPONENT_DIRS` pointing
here). ESP-IDF v5.5.6, C. The 68000 machines use Musashi (the LC and II
as a 68020: `CONFIG_M68K_68020`, with `m68k_set_fpu()`; the LC II and LC
III as a 68030: `CONFIG_M68K_68030`; the Amiga's 68000 or 68EC020 chosen at
run time: `CONFIG_M68K_RUNTIME`; a machine that never calls
`m68k_bus_error()` sets `CONFIG_M68K_NO_BUS_ERRORS`, the Amiga: the 68010
and up otherwise copy their 16 registers before every instruction, to
restart it after one; `CONFIG_M68K_PROGRAM_READS`, Musashi's
`M68K_SEPARATE_READS`: instruction words through the machine's
`m68k_read_immediate_16/32()`, PC-relative operands through
`m68k_read_pcrelative_8/16/32()`, the Amiga's 68EC020 instruction cache;
`CONFIG_M68K_PREFETCH`, Musashi's `M68K_EMULATE_PREFETCH` with the opcode
fetch through it too: the next word fetched ahead, so an instruction
writing over the next one runs the old word, the Amiga's Copylock-style
decoders),
the Z80 ones the
Z80 core (the MSX with its `CONFIG_Z80_MSX` memory), the Commodores and the
Atari XL the 6502 core (the C128 the Z80 too), the IIGS the 65C816 core, the
Archimedes the ARM2 core, the PC the x86 core; the CPC and the +3 share the uPD765.

Changes here affect every emulator: build all of them after any change, the
eleven on both boards and the four for the Tab5 only (26 builds; only the
components a project requires get built: MINIMAL_BUILD).

## The two boards

- **MaTouch** (`/dev/cu.usbmodem1101`): `idf.py build`, `sdkconfig`, `build/`.
  Every MaTouch project has `CONFIG_EMU_SPEAKER` on (2026-11-16, the user's
  choice: they connect the board's USB-to-UART port): once a machine starts,
  the console and flashing are on that port (`/dev/cu.wchusbserial*`,
  921600 baud), the native USB port is gone; at the setup screen the console
  is still on native USB.
- **Tab5** (`/dev/cu.usbmodem3101`, ESP32-P4 rev v1.0, ILI9881C + GT911
  version): `idf.py -B build-tab5 -DIDF_TARGET=esp32p4
  -DSDKCONFIG=$PWD/build-tab5/sdkconfig build` (and `-p PORT flash`); its
  sdkconfig lives in the build folder, from the project's
  `sdkconfig.defaults` then `boards/m5stack_tab5/sdkconfig.defaults` (the
  projects' CMakeLists.txt). After changing either, delete
  `build-tab5/sdkconfig` and `reconfigure`. Its components (BSP, esp_hosted,
  esp_wifi_remote, ...) have their own lock file, `dependencies.lock.esp32p4`.
- Both builds share the project's `managed_components/`: each target's solve
  removes the other's, which then come back from the component manager's
  cache, and that build compiles everything again (~2.5 min).
- Both boards running the same project have the same mDNS name: one of them
  becomes `<name>-2.local`, and after more restarts `-3`, `-4`, while the
  Mac's cache still answers the old names (`/api/info` says which board:
  `"board"`; the status's `board.name`, the web page's first Board row).
- The board code: `include/emu/board.h` picks `board_matouch7.h` or
  `board_tab5.h`; `lcd.c` (RGB panel) or `lcd_tab5.c` (MIPI-DSI through the
  BSP); `touch.c`, `sdcard.c`, `speaker.c` have both; `tab5.c` (charging, the
  C6, esp_hosted, the RTC), `tab5_keyboard.c` and `usb_hid.c` (USB keyboards
  and mice on its USB-A port: the BSP's USB host, espressif/usb_host_hid in
  boot protocol, the same callbacks as Bluetooth's; menuconfig
  `EMU_USB_HID`) only build for the Tab5. The HID driver uses the managed
  `espressif/usb` the BSP brings (on ESP-IDF 5.5 it stands in for the
  IDF's `usb`); the USB-C console (USB-Serial-JTAG) keeps working.
- `BOARD_LCD_H_RES` x `BOARD_LCD_V_RES` is the landscape picture everything
  draws (presenter, setup screen, `L` dumps, touches): 1024x600 or 1280x720.
  On the Tab5 `lcd_flush(y0, y1)` has the PPA turn those rows onto the
  720x1280 portrait panel; landscape (x, y) is panel (y, 1279 - x), the
  orientation esp32_basic uses with the Tab5 Keyboard below the screen. A
  self-test at start checks it ("PPA rotation is not the expected one").
- The setup screen is laid out for 1024x600: `SX()`/`SY()` scale it (exact
  on the MaTouch), `FONT_BIG` is 34 px on the Tab5, the default font 28.
  Machines' `view_width` is for 600 lines: `emu_view_width()` scales it.

## ESP-IDF 5.5.6 (2026-11-16)

Every project builds with 5.5.6 (from 5.5.4, since removed).
The same compiler (esp-14.2.0_20260121).

- **Install**: `/Applications/eim.app/Contents/MacOS/eim install -i v5.5.6
  -t esp32s3,esp32p4 -p $HOME/.espressif -n true --do-not-track true`
  (EIM sends usage data unless told not to). The build commands stay as
  they were (`$IDF_PATH/export.sh`, `IDF_PATH` from `~/.zprofile`, now
  v5.5.6), with the installer's Python: `tools/python/v5.5.6/venv`.
- **Build folders made with 5.5.4 must start afresh**: `idf.py` refuses a
  folder configured with another Python ("Run 'idf.py fullclean'"), and the
  bootloader sub-build keeps the old ESP-IDF path in its own CMake cache.
  `fullclean` also deletes `build-tab5/sdkconfig` (it lives in the build
  folder): keep a copy and put it back.
- **The two copies** (`components/esp_lcd`, `components/fatfs`) were
  taken again from 5.5.6, with our changes applied as patches made from
  the old copies against 5.5.4 (`diff -ru`, `patch -p0`): both applied
  cleanly. 5.5.6 brought FatFs's exFAT/FAT32 fixes (CVE-2026-6682, -6683,
  -6685, -6687), and still has the RGB bounce-buffer bug (our change
  stays; `x 5/10/20` on the MaTouch: each put right at the next VSYNC).
- **What changed** (5.5.5 and 5.5.6, from the commit titles between the
  tags; the GitHub notes are short and release-notes.espressif.com needs
  JavaScript): many Wi-Fi and NimBLE fixes (HID service discovery, bond
  store, crashes on disconnect, CVEs), Wi-Fi with PSRAM and
  `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` fixes, SD/FatFs, HTTP server and
  esp_event security fixes, `esp_timer` races; P4: the second core's branch
  predictor reaching PSRAM with the cache off, MIPI-DSI signal integrity,
  PPA/DMA2D races, USB host security fixes.
- **New defaults**: NimBLE pairing hardening (e.g.
  `BT_NIMBLE_SMP_REMOVE_BOND_ON_PAIR_FAIL_AS_CENTRAL`: a failed pairing
  drops the bond). Bluetooth keyboards and mice not tried yet.
- **Components**: the solve moved only the Tab5's mdns 1.14.0 -> 1.14.1
  and usb 1.6.0 -> 1.6.1 (esp32ste already had them).
- **The internal DMA reserve is shared now** (5.5.5): the pool
  `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` sets aside is added with
  `MALLOC_CAP_DEFAULT` at its lowest priority (`esp_psram.c`,
  `esp_psram_extram_reserve_dma_pool()`), so plain `malloc()` takes from
  it once the rest of internal RAM is short. Measured with esp32ste, TOS
  2.06 4 MB high res: MaTouch 77-79% of real time (5.5.4: 76-77%),
  internal RAM free while running 57.3 KB (64.5 KB), minimum 39.5 KB
  (46.8 KB); during a 720 KB upload and download on the web page the
  minimum fell to 32.1 KB (5.5.4: unchanged, 46.9 KB). Both transfers
  worked. Tab5 (TOS 1.62): 100%, core 1 43% (42%), internal minimum 97.7
  KB. If something runs out of internal or DMA memory on the MaTouch
  (Wi-Fi, Bluetooth, the SD card), look here first. The other projects
  were built, not tried on the boards.

## ESP-IDF 6.1 (2026-11-16)

ESP-IDF 6.1.1 is installed beside 5.5.6 (by Espressif's installer,
EIM). esp32ste builds with either; the other projects still
name `components/` and build with 5.5 only, until they include
`components.cmake`.

**Decision (the user, 2026-11-16): stay on 5.5** (5.5.6 since that day,
"ESP-IDF 5.5.6" below). The boards run 5.5 builds; esp32ste keeps building
with both, as the test case for 6.x. Move
the others when there's a concrete reason (a component or fix that needs
6.x, 5.5's end of support), and before that profile the MaTouch's last ~2
points (below) and its internal RAM.

- **Environment**: `export.sh` fails (EIM's tools aren't where it looks);
  source `~/.espressif/tools/activate_idf_v6.1.1.sh`, which refuses to run
  from a script file, so: `bash -c '. ~/.espressif/tools/activate_idf_v6.1.1.sh
  >/dev/null 2>&1; unset IDF_COMPONENT_LOCAL_STORAGE_URL; idf.py ...'`. It
  points the component manager at a local mirror (older versions:
  esp_codec_dev 1.5.8 instead of 1.5.11): unset it.
- **Separate everything** from the 5.5 builds: build folders
  (`build-idf6`, `build-tab5-idf6`), `-DSDKCONFIG` in them for both boards,
  lock files (`dependencies.lock.idf6`, `.esp32p4.idf6`, set in esp32ste's
  CMakeLists.txt). Never configure two builds of one project at once: they
  race in `managed_components/` (FileExistsError, missing sources).
- **Code changes needed**: the two forks (`idf6/`), and `lcd.c`'s
  `bits_per_pixel` (`in_color_format` on 6). Nothing else in emu, Musashi
  or the ST core; GCC 15.2 and C23 gave no new warnings, with picolibc and
  warnings as errors (6.x defaults). The Tab5's BSP 1.3.0, esp_hosted
  2.12.9, esp_lvgl_port 2.8 (a deprecation warning: `on_refresh_done`) build
  unchanged.
- **New defaults** (a fresh sdkconfig; an existing one keeps the old):
  picolibc (no speed cost, ~7 KB more internal RAM free than newlib),
  default warnings and orphan sections are errors, FreeRTOS and ring
  buffers out of IRAM. The last two cost the MaTouch's ST 5 points of real
  time from PSRAM (TOS 2.06 4 MB high res desktop: 5.5.4 76-77%, 6.1 69-70%,
  `CONFIG_FREERTOS_IN_IRAM` 73%, + `CONFIG_RINGBUF_IN_IRAM` 73-75%; the
  console's UART driver uses ring buffers). esp32ste sets both; the ~2
  points left are GCC 15 or ESP-IDF itself (same flags but
  `-fzero-init-padding-bits=all`, `-fno-malloc-dce`). Tab5: no change (core
  1 40-42% on the TOS 1.62 desktop either way).
- **The RGB fix upstream**: 6.1 resets `bb_eof_count` at each VSYNC
  restart (the core of our change) but doesn't refill the bounce buffers
  after a frame out of step; `idf6/esp_lcd` adds that and the counter.
  MaTouch, `x 5/10/20`: each stall put right at the next VSYNC.
- **`idf.py flash`** writes only the regions changed since the last flash
  from that build folder, then verifies; after another project, use
  `flash -a` (everything).
- MaTouch internal RAM on 6.1 (both options on, picolibc): ~60 KB free
  while running against 64.5 KB on 5.5.4; the minimum 39.8 KB (47 KB).

## Layout

| Path | What |
|---|---|
| `components/musashi/` | 68000 core (Musashi, evansm7 fork) + `m68kconf.h` with the `m68k_host_*()` hooks; `test/` host tests (`make -C components/musashi/test`) |
| `components/z80/` | Z80 core (from the CPC): T-states with optional ZX contention, or CPC microseconds (menuconfig "Z80 core"); `Z80_READ_SPECIAL` sends reads of NULL banks to the bus (the C128's MMU); `Z80_MSX`: eight 8 KB banks (`Z80_BANK_SHIFT` 13) and accesses to `$FFFF` to `read_special`/`write_special` while `z80_ffff_special` is set (an expanded slot's subslot register); `test/` zexdoc/zexall host test for each build (`z80_test_msx` too) |
| `components/w65816/` | 65C816 core (the IIGS): `rd`/`wr` tables of 65,536 page pointers, NULL to the bus callbacks (which may add `penalty` cycles and call `w65816_end_run()`); `vector_bank` (the IIGS reads vectors from its ROM, bank $FF); the code page cached as a pointer (`refill()`); helpers left to the compiler and `-Os` (forced inline it was 72 KB, twice the S3's instruction cache: ~850 cycles an instruction); `test/`: SingleStepTests (`ssts`), interrupts (`irq`), run against step (`diff`), `speed` |
| `components/m6502/` | 6502 core (from the Commodore): every bus cycle, all 256 opcodes, several CPUs at once, `m6502_run()` to the machine's next event; stalls from `stall_from` (reads only, or writes too with `stall_writes`: the Atari's HALT), and for the Atari's ANTIC tables of a line's busy and free cycles (`dma_free`, `dma_busy`, `dma_base`, `dma_len`, `dma_stop`) that it skips by itself, calling `bus.stall()` only past them or at `dma_stop` (WSYNC); `linker.lf` keeps its jump table in internal RAM; `test/` Klaus Dormann's functional test and an opcode cycle table (`make -C components/m6502/test`) |
| `components/m6809/` | 6809 and 6309 core (the CoCo): instruction level with each instruction's documented cycles (the 6809's, the 6309's in emulation or native mode), all of the 6309 (E F W Q V MD, native mode stacking W, FIRQ with FM, TFM, the bit and register-register instructions, DIVD/DIVQ/MULD, the traps at `$FFF0`), the 6809's undocumented opcodes as commonly described; memory through 256-byte `rd`/`wr` pages (NULL: the bus callbacks), IRQ/FIRQ levels, NMI edges (armed once S is loaded), HALT, CWAI/SYNC; `M6809_HOST_HOOK` for host tracing; `linker.lf` keeps its jump tables in internal RAM; disassembler and debugger adapter; `test/`: table-driven cases and every opcode's cycles and length against the data sheet (`make -C components/m6809/test test`) |
| `components/arm2/` | ARM2 core (the Archimedes): ARMv2 with the 26-bit PC/PSR in R15, the four modes' banked registers, exceptions and aborts, LDM/STM with ^, LDRT/STRT, rotated unaligned LDR (not LDM), SWP for an ARM250/ARM3 (`swp`), coprocessor instructions to the machine; memory through a table of 4 KB pages (host address + access bits: user/privileged read/write, slow, watched), the rest to the bus callbacks (which may abort, add wait states to `left`, call `arm2_end_run()`: the run then ends with the ticks it used); time in memory-clock ticks (S 1, N 2, I 1; `slow_s`/`slow_n` for ROM). `arm2_run()` is a threaded interpreter: one 4096-entry label table on bits 31-20 (the AL row a handler per data processing opcode and operand form, LDR/STR by size and offset, LDM/STM, B/BL; the other conditions through `cond_mask`, kept as the flags change; NV skipped), the PC, ticks and the fast page (`run_t.page`: the PC's, or ~0 while an interrupt is pending or the debugger is on) in registers, written back around callbacks; `ARM2_HOST_HOOK` for host tracing; `linker.lf` keeps its tables in internal RAM (the label table is 16 KB); disassembler (RISC OS style) and debugger adapter (`arch` "arm2", 26-bit addresses mapped by page); `test/`: `cases` (190), `diff.py` against Unicorn, `bench` (`make -C components/arm2/test test`; host MIPS don't predict the Tab5, see esp32arc's lessons) |
| `components/x86/` | x86 core (the PC): the 8088, 8086 and 80286 as models of one core (the 8086s' aliases, shift counts, PUSH SP, FLAGS bits, divide error return, word wrap at FFFF and REP resume at the last prefix; the 286's 186 instructions, #UD, faults by longjmp, the 286's protected mode: descriptors, limits and rights (real mode's limits too: LOADALL's "unreal" segments), the segments' direct windows cut to their limits so the slow paths check them, CPL, call/interrupt/trap/task gates, task switches through the TSS, exceptions with error codes (`fault_err()`), the double fault and the shutdown (`bus.shutdown`, else `stuck`), IOPL, LLDT/LTR/LAR/LSL/VERR/VERW/ARPL/LOADALL, #NM from EM/TS; the 8087/287 coprocessor in `x87.c` (`x86_set_fpu()`, `bus.fpu_error`; 80-bit registers kept exactly, FADD/FSUB/FMUL/FDIV/FSQRT exact with 128-bit intermediates, the transcendental functions through doubles; x86.c reads an ESC instruction's operand bytes, the written ones too, before it runs); the far transfers and interrupts go to cold `pm_*` functions that check everything before they change anything (a task switch's later faults are the new task's); lazy flags, 4 KB pages over 16 MB (host address + `X86_NO_READ`/`X86_NO_WRITE`, the rest to the bus callbacks); time in CPU clocks (documented clocks, EA times, the 8088's word transfers, a prefetch queue model: within 3.1 clocks of the chip on average); the BIOS trap `FE 38 nn` only inside `[trap_lo, trap_hi)`; the 8088/8086's undefined flags as the chips leave them (DIV/IDIV through their CORD microcode loop, MUL/IMUL, the BCD instructions, after MartyPC, MIT). Speed: one run loop per model with the instruction set inlined (`run_286()` in internal RAM on the P4, `X86_HOT`; the 8088/8086's and a debugger loop, taken when breakpoints are set at a run's start, in flash), the instruction's bytes through a host pointer kept in registers (`code_t`; the code window per page, `fetch_slow()` past it), each segment's direct window (worked out lazily at the first access after a load, cached by base; `x86_flush()` when the page table or A20 changes), the 286's near jumps in the window moving the pointer, REP MOVS/STOS in bulk on the 286; `X86_HOST_HOOK`; `linker.lf` keeps its jump tables in internal RAM; disassembler (DOS DEBUG style); `test/`: SingleStepTests 8088 (3,007,000 tests) and 8086 (646,000) all pass with the undefined flags compared, the 286's real mode suite (1,478,000) but for 17 (protected mode: esp32pc's `host/tests/pm.S` and Minix 2.0.4); `-w` runs them with writable pages (the direct writes) (`make -C components/x86/test`, `make data` fetches the suites, ~0.9 GB, not in git; esp32pc's CLAUDE.md has the speed lessons) |
| `components/emu8950/` | Mitsutaka Okazaki's emu8950 (MIT, v1.2.2: YM3812/Y8950, the PC's AdLib), unchanged but for `include/`; `calloc`/`malloc` renamed to PSRAM-preferring wrappers and its .bss in PSRAM (`linker.lf`, when the project sets `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`); run it at the chip's rate (its rate converter uses doubles) |
| `components/upd765/` | uPD765A floppy controller with DSK/EDSK images (CPC, +3), and the PC's wiring (`upd765_wire()`: DMA with terminal count, the INT line, four units, multi-track; raw images through callbacks). The INT line falls at each SENSE INTERRUPT STATUS (IBM's XT BIOS asks once after a reset, then waits for the next seek's interrupt); on the PC an empty drive is still ready (its line tied high); `upd765_disk_track_ids()`/`upd765_disk_read()` read a DSK's sectors by ID (the free space) |
| `components/diskspace/` | Free space on disk images as their own filesystems count it, for the web page's drive bars (`include/diskspace.h`): FAT12/16/32 and MBR disks (`fat.c`: ST, MSX, PC, Archimedes DOS), MFS/HFS/HFS+, partition maps, ProDOS, DOS 3.3 (`apple.c`), CP/M 2.2/3 (`cpm.c`: CPC, +3), TR-DOS, D64/D71/D81 BAM (`cbm.c`), Atari DOS 2/2.5/MyDOS/SpartaDOS, RS-DOS and OS-9 (`coco.c`), ADFS old and new map (`acorn.c`); each reads through the machine's callback by offset and refuses figures that don't add up. The machines count where the image is safe to read (the emulation task or with it paused: at insert, a second after the last write, or after a flush) and keep the answer for `status()`. `test/`: a program per family with images made in it, matched against the machines' own DOS figures on the host (`make -C components/diskspace/test`). The Amiga counts its own (esp32ami's `space.c`) |
| `components/emudbg/` | The debugger for programs in the machines (`docs/debugger.md`, the VS Code extension `../esp32emu-vscode`): `emudbg.c` the protocol, breakpoints and steps (platform independent), `emudbg_freertos.c` the board's TCP server (port 6502, core 0), `emudbg_posix.c` the host builds' (`<M>_DEBUG=<port>`, `emudbg_host_run()`); `test/`: `dbg_test` (the commands and steps against the 6502 and Z80 cores) and `demo`, a bare 6502 or Z80 with RAM serving the debugger (the extension's tests run against it). The cores' adapters: `m6502_dbg.c`, `z80_dbg.c`, `w65816_dbg.c`, `m6809/m6809_dbg.c`, `musashi/m68k_dbg.c`, `arm2/arm2_dbg.c` |
| `components/emu/include/emu/emu.h` | The machine interface (`emu_machine_t`) and runtime API |
| `components/emu/emu.c` | Boot order, emulation task (core 1), presenter (its drawing in internal RAM on the S3, `PRESENT_HOT`: from flash/PSRAM it took the shared instruction cache from core 1's emulation, 3-6% of the PC's Wolfenstein 3D), stats, temperature, supervisor (touch: setup corner and machine touches), pausing |
| `components/emu/setup_ui.c` | LVGL setup screen (status, WiFi, password, Bluetooth, pairing, drives, images) |
| `components/emu/web.c` + `web/index.html` | Web server and the page (one page for all machines: it reads `/api/info`) |
| `components/emu/disks.c` | Disk image folder, drives, NVS memory of inserted disks, blank disks |
| `components/emu/console.c` | Serial console (`include/emu/console.h` lists the commands), with `p [samples] [core]`: a 20 kHz sampling profiler of a core (1 the emulation, 0 the presenter, WiFi, ...) |
| `components/emu/ble_hid.c` | BLE HID host: pairing, reconnecting, reports -> HID usages |
| `components/emu/usb_hid.c` | Tab5: USB keyboards and mice (boot protocol) on the USB-A port, the same callbacks as BLE's |
| `components/emu/net.c` | WiFi station + SNTP |
| `components/emu/lcd.c`, `lcd_tab5.c`, `touch.c`, `sdcard.c`, `speaker.c` | Board drivers; `include/emu/board.h` picks the board's header |
| `components/emu/tab5.c`, `tab5_keyboard.c` | Tab5: battery charging and its state (`battery.h`), the ESP32-C6 (power, esp_hosted, its Bluetooth controller), the Tab5 Keyboard |
| `components/fatfs/` | ESP-IDF 5.5.6's fatfs with `FF_FS_EXFAT` 1 (from esp32_basic): exFAT SD cards. Overrides ESP-IDF's for every project: update it with ESP-IDF |
| `components/esp_lcd/` | ESP-IDF 5.5.6's esp_lcd (without test_apps), one change marked `esp32emu` in `rgb/esp_lcd_panel_rgb.c` (+ `emu_lcd_rgb_resyncs` in its header): see the lessons. Overrides ESP-IDF's for every project and both boards (a `reconfigure` picks it up): update it with ESP-IDF |
| `idf6/esp_lcd/`, `idf6/fatfs/` | The same two for ESP-IDF 6 (6.1.1's, the same changes: `FF_FS_EXFAT` 1 and the guards in `ffconf.h`; the resync check in `lcd_rgb_panel_try_restart_transmission()`). See "ESP-IDF 6.1" |
| `components.cmake` | A project's `EXTRA_COMPONENT_DIRS`, included before ESP-IDF's `project.cmake`: `components/`, or with ESP-IDF 6 its folders but `esp_lcd` and `fatfs`, plus `idf6/` |
| `boards/m5stack_tab5/sdkconfig.defaults` | The Tab5's settings (PSRAM, esp_hosted SDIO pins, NimBLE over VHCI, fonts, speaker on) |
| `components/emu/json.c` | Small JSON builder for the web API |
| `tools/emu_console.py` | Console client (run with the IDF venv's Python, `-I`); `profile ELF [N [TOP [CORE]]]` runs `p` and names the hottest functions (the ROM's too, from the chip's ROM ELF; a static name in several files gets its file) |
| `tools/sd_folders.ps1` | An SD card's shared folders (until 2026-11-17: `roms`, `floppies`, `disks`, `atari`) into each machine's own (`<folder>/roms` for its cartridges and the ROMs that override the built-in ones, the cartridges from `cbm`, `coco`, `cpc`, `msx` there too; the Macs share `mac`), as rpi5emu's `tools/data_folders.sh` does the Pi's data folder. `-DryRun` only says. Runs on Linux too (`pwsh` 7.6, a .NET global tool on the PC: `dotnet tool install --global PowerShell --version 7.6.10`; 7.7.0's Linux package is missing on NuGet): checked 2026-11-18 on a made-up card with a file for every rule, nested folders right, a name already there kept |

## How a machine plugs in

- `emu_run(&machine)` from `app_main`. Order: NVS, `reserve_memory()` (the
  machine's big PSRAM blocks first), (Tab5: the clock from the RTC,
  charging, C6 power), SD card ->
  `storage(sd)`, disks restored, LCD, touch, console, (Tab5: esp_hosted),
  WiFi, web, (Tab5: the C6's Bluetooth controller), BLE, (Tab5 Keyboard),
  stats/temperature tasks, setup screen, `start()`; the setup screen comes back with `emu_set_message()` text when
  `start()` fails (e.g. no ROM).
- `start()` may be called again (setup screen choices changed, console):
  always with the emulation paused. If it can't get its memory any more,
  `emu_reboot_into_machine()` restarts the board straight into the machine.
- Setup screen choices: `get`/`set` (set saves). The buttons don't name the
  machine: Start before it starts, Back while it's paused, Reset at the top
  right. Changing a choice while running turns Back into Restart; if the
  restart fails, the runtime sets
  the old choices back and starts again. A `live` choice (the ST's CPU
  speed, Tab5 only) changes the machine as it's set, no restart; `set()` may run on
  another task, so the machine picks the value up between frames. The
  choice rows are in a box that scrolls (the user's request once the Amiga
  had five): as tall as the rows, or what the card's other lines leave
  (`fit_choices()`), so up to three look as before and more scroll
  instead of shrinking. Three rows fit closer (`tight` in setup_ui.c:
  54-pixel rows), four or more (the Archimedes, CoCo, PC, Amiga) keep
  those rows in a taller card (down to just above the buttons) with less
  padding, and the last ones show cut off under the others with the
  scrollbar beside them. Buttons choose on release (`CLICK_TRIG`) and a
  drag that starts scrolling drops its button (`choices_scrolled()`):
  LVGL's matrix would choose on press, and still sends RELEASED after a
  scroll. Up to six rows (`EMU_UI_MAX_CHOICES`). The buttons share 560 pixels; a row with a
  single button (the IIGS's memory on the MaTouch, shown but not choosable)
  gets one button's width in a row of four (140). A row whose buttons
  another choice decides (the MSX's memory: one 64 KB button for an MSX1,
  the mapper sizes for an MSX2) comes from the machine's `choice_names(i)`
  (NULL: the row's own names); the rows follow whenever a choice changes.
  It's a machine field, not an `emu_choice_t` one: the projects fill their
  choice rows positionally, and a new field there warns (-Wextra).
- A button name with a `\n` gets a second, smaller line under the first
  (`FONT_NOTE`: montserrat 20 on the Tab5, 12 on the MaTouch, which
  `CONFIG_EMU_SETUP_SCREEN` selects: an existing build needs `idf.py
  reconfigure` once, else it doesn't find them; at 3/4 of the text's opacity, readable on the checked
  button's blue too): the Macs' Monitor buttons, "640 x 480\nup to 256
  colours", their names per model from `choice_names()`. The button matrix
  draws a button's text as one label, so `choice_draw_task()`
  (`LV_EVENT_DRAW_TASK_ADDED`) makes that one transparent and draws a label
  for each line around its centre. Names without one look as before. Two
  lines fit a three-row card's rows (54 px, 64 on the Tab5: 30 + 27).
- **The Sound row** (the user's request, 2026-10-22: sound sometimes too
  loud, no way to lower it): with `CONFIG_EMU_SPEAKER` and a machine with
  `audio_render` (every one), the setup screen adds a last row of its own
  after the machine's choices (it counts in `tight`/`more`, not in
  `EMU_UI_MAX_CHOICES`): a mute button where the buttons begin (grey with
  a speaker, blue and a speaker with an X while muted) and a slider with
  its value (0-100). Both live; moving the slider unmutes; the slider goes
  grey while muted. The samples are scaled by the slider's square (an even
  feel), in `speaker.c` (the Tab5's codec stays at full scale);
  `CONFIG_EMU_SPEAKER_VOLUME` (60% of full scale) shows as 77 until one is
  set. NVS namespace `speaker` (`volume`, `muted`), the same for every
  project on the board, written as the setup screen closes (and by the
  console's `v [0-100|m]`). LVGL 9.4's slider moves only after 10 px of
  sideways drag and leaves a drag that scrolls the rows alone (checked:
  a swipe starting on it scrolled the Amiga's rows, the volume unchanged).
  Checked on the Tab5 (esp32ami: mute, the slider, restored after a
  restart); the loudness itself only by ear.
- Sound: the speaker's task counts the frames it had to make up when the
  machine was late (`speaker_missing_frames()`: dropouts heard as
  artefacts); the IIGS shows it with `i`.
- Inputs arrive from other tasks: `hid_key(usage, pressed)` (modifiers as
  usages $E0-$E7, each side separately), `hid_mouse(dx, dy, buttons)`,
  `touch(x, y, pressed)` in machine screen pixels (only if the machine sets
  it; the top-right setup corner is never forwarded).
- **The touchpad** (`touchpad(dx, dy, buttons)`, wins over `touch`): the
  touch screen drives the machine's mouse, the user's choice over the
  pointer jumping under the finger (2026-10-21: at higher resolutions the
  pointer's tip stays in sight, and a tap anywhere clicks). The
  supervisor's state machine (esp32ami's, which the user liked): touches
  anywhere but the setup corner; moving more than `TOUCHPAD_SLOP` (the
  panel's height / 90) moves the pointer as far as the finger goes on the
  picture (moves in machine pixels, the fractions kept); a tap clicks the
  left button (held 60 ms; the supervisor waits); touching again within
  300 ms of a tap drags with it; holding still 0.5 s holds the right one
  until the finger lifts. Each machine turns pixels into its mouse's
  counts (the ST, Macs, CPC: one each; the IIGS halves 320 mode's; the
  Amiga halves super-hires; the PC's mickeys of 640 x 400; the
  Archimedes' RISC OS 3's 3 OS units a count; the CoCo's joystick steps)
  and maps the right button (the Macs and IIGS: their one button, the
  Archimedes: Menu). Console tests: `t x y [ms]` taps, `t x y ms x2 y2`
  moves (the duration third). Checked on the Tab5 (2026-10-21) with those,
  screenshots measuring the pointer: slow drags move it as far as the
  finger on the picture within 1-3% on the Amiga, ST, Archimedes
  (640x480), IIGS (640 mode) and Mac LC (fast ones get the machine's own
  acceleration: Intuition, ADB); taps anywhere click (double taps open a
  disk on all five), a tap and a drag moves a window (Amiga, ST), the long
  press holds the right button (Workbench's menu bar), Menu (RISC OS's
  Filer menu), the one button (GS/OS's File menu); the setup corner still
  opens the setup screen. Not on the board: the MaTouch, the PC, CPC,
  CoCo, Mac Plus, LC III.
- Drives of different kinds (the CPC's floppies and cartridge slot): the
  up to 5 drives (`DISKS_MAX_DRIVES`; the CoCo's two floppies, hard disk,
  tape and cartridge: the drives page lays out five closer, 4 or fewer as
  before); optional `disks_config_t.accepts(drive, name)` limits the setup screen's
  image list for a drive, `takes_blank(drive)` its "New" button. The web
  page offers every drive; `insert()` refuses what doesn't fit.
- **Its own folder on the SD card** (`disks_config_t.folder`; one per
  machine since 2026-11-17, the user's wish: `zx cpc cbm coco apple2gs msx
  xl st pc arc amiga`, the three Macs sharing `mac`): its images, and in
  its `roms/` (since 2026-11-18, the user's call: the built-in ROMs come
  from the project's `roms/` through `idf.py flash`) the cartridges
  (`disks_config_t.cart_extensions`: the CBM's, CoCo's, CPC's and MSX's;
  `disks_path()` puts a name with one of them there, `disks_list()` lists
  them by plain name, only those some drive `accepts()`) and ROM files
  that override the built-in ones (`ROM_FOLDER` in `rom_store.c`, e.g.
  `SDCARD_MOUNT_POINT "/zx/roms"`, read before the flash partition; the
  cartridge drives' `accepts()` refuses their names: `rom_store_is_rom()`).
  Shared before: `roms/` for all, `floppies/` for the ST,
  Archimedes and Amiga (whose `.adf` and `.hdf` look alike), `disks/` for
  the Macs, the XL's `atari/`; `tools/sd_folders.ps1` moves an old card.
  The Pi's data folder is laid out the same (rpi5emu's `emu_rom_dir()`).
- **Offers for a disk** (optional, `disks_config_t.offer(drive)` and
  `take_offer(drive)`, the user's request 2026-10-22 for the Amiga's RP9
  games): a sentence for what the machine would do for the disk in a
  drive ("Ants wants an A500+: 1 MB chip RAM, ..."), NULL for nothing;
  taking it changes the machine's settings. The setup screen's disk page
  shows the first drive's offer on its status line and a button at the top
  right ("Set up and restart", before the first start "Set up and
  start"): it takes the offer and closes the screen, whose caller then
  restarts as for changed choices (the old ones back if it can't start).
  The web page shows each under its drive with a button: `/api/status`'s
  `offers` (`drive`, `text`), `POST /api/drive/<n>/offer` through
  `emu_take_offer()` (the same restart; a machine not started yet only
  gets the settings). offer() is asked every second by the web page:
  keep it cheap (the Amiga reads the manifest as the disk goes in).
- Disk changes from the setup screen, web page and console run inside
  `emu_paused()` (between frames). A machine that ejects a disk itself calls
  `disks_ejected(drive)` from the emulation task; NVS is written later from
  the stats task.
- Web page script (optional): `web_script`..`web_script_end` (usually an
  `EMBED_FILES` file) is served as `/machine.js`; `/api/info` says
  `"script": true` and the page loads it before setting up the drop area.
  It calls `registerConverter({description, extensions, convert(name,
  bytes, {isImage, log})})`, which returns `[{name, data}]` images: files
  with those extensions (also inside ZIP files) are converted in the browser,
  then uploaded as usual. The Mac uses it for StuffIt/BinHex archives.
  It can also call `registerPacker({description, pack(name, files, ctx)})`
  (the PC's): the files that are neither images nor a converter's (loose,
  from dropped folders, whose paths `dropped()` reads through
  `webkitGetAsEntry`, or a ZIP's, with their folders) are packed together
  into images `[{name, data}]`, named after their common top folder, the
  ZIP or "FILES". With a packer the file picker takes any file.
- Folders as drives (optional, `disks_config_t.folders`, the PC's): the
  disk folder's subfolders are listed too, as `NAME/`; `insert()` gets the
  path with the slash; `disks_can_insert()` (the web's drive PUT) accepts
  them; `/api/disks` marks them `"folder": true` and the page offers only
  Insert for them (no download, replace or delete).
- Uploads from the page go through `XMLHttpRequest` (`put()` in index.html):
  `fetch()` can't report an upload's progress. The log line shows the
  percentage, rate and time left over a bar, then "writing it to the SD
  card" until the board answers (the Tab5 takes the whole file into PSRAM
  first). A 20 MB image to the Tab5: 0.5-1.9 MB/s.
- Status: `status(j, EMU_STATUS_ROWS)` adds `emu_json_row()`s for the
  machine card (`emu_json_row_bar()` with a percentage: a bar, e.g. free
  memory), `status(j, EMU_STATUS_DRIVES)` adds `emu_json_drive()`s
  (`emu_json_drive_space()` with the disk's free space and size: "N free
  of M" and a bar under the drive; every machine reports it, checked on
  the Tab5 2026-10-21, the C64 on the host only). status() runs on the
  HTTP task: it only reads figures the machine keeps.
- Presenter: `render_line()` at the machine's resolution, scaled to
  `emu_view_width()` x the panel's height (centred) when
  `screen_signature()` changes, then `lcd_flush()` of the rows it wrote;
  `EMU_SCALE_SMOOTH` filters (1-bit pictures at odd factors), `NEAREST` doesn't.
  A frame may say otherwise (`emu_screen_t`, 0 for the default): its size
  on the panel (`view_w`, `view_h`, centred, the margins cleared when it
  changes) and the columns shown (`src_x`, `src_w`); the Amiga's
  super-hires frames, 1440 wide, show 1280 columns one to one. Sources up
  to 1536 wide on the Tab5 (1024 on the MaTouch, whose presenter buffers
  stay at the machine's own width: internal RAM). Touches follow the
  presenter's last geometry. Callers zero their `emu_screen_t`.

- Debugger (`emudbg.h`): the machine core (not `main/`, so the host build
  has it too) fills an `emudbg_machine_t` with its CPUs (`emudbg_cpu_t`: a
  name, the core's adapter `emudbg_arch_6502`/`_z80`/`_65c816`/`_68000`/`_6809`/`_arm2`,
  the core's state, peek/poke when the core can't see memory by itself:
  the 68000s, the C128's Z80, the IIGS's shadowed RAM), `where()` (frame,
  line, cycle), `reset`, `screen`/`render_line`, `type`, and calls
  `emudbg_set_machine()` at the end of every init: a start clears the
  cores' maps and the CPUs may differ (C64 or C128, drives). The runtime
  calls `emudbg_frame_end()` after each frame, `emudbg_rearm()` after a
  reset, `emudbg_suspend()`/`emudbg_resume()` around pauses. The host
  harness ends its setup with `emudbg_host_run("<M>_DEBUG", frame, hz)`.

## The Raspberry Pi side (`../rpi5emu`)

Each esp32* emulator is getting an rpi5* sibling for the Raspberry Pi 5
(the user's plan, 2026-11-15; `../rpi5zx` first): rpi5emu is this
runtime's Linux counterpart (SDL 3, the same `emu_machine_t`), and the
rpi5* projects compile the esp32* machine cores and this repository's CPU
cores (`components/*`) with CMake. A change to a core or to a machine's
core reaches the Pi too: build the affected rpi5* project as well
(`../rpi5emu/CLAUDE.md`). rpi5emu serves this repository's
`web/index.html` with the same API; the page takes optional fields for a
host that isn't a board (`/api/info`: `host`, `storage`,
`storage_title`, `about`; `/api/status`: `board.title`, `board.rows`
instead of the board's own rows, no `bluetooth`: its card hidden). The
boards send none of them and their page reads as before.

## Lessons (from the STE, where most of this code was written)

- **`emu_set_message()` keeps 255 characters** (2026-11-17; 127 before):
  the machines' "No ROM: put it in roms/ and flash with idf.py flash, or
  copy it into the "mac/roms" folder on the SD card." texts ran to ~180,
  and the old buffer cut them off just where they said where the file goes
  (since 2026-11-18 they name roms/ and idf.py flash only).

- **Dependency solves broke on registry drift (2026-11-16)**: a project
  whose lock no longer matched `components/emu/idf_component.yml` re-solved
  and got mdns 1.15.1, which stops the MaTouch build (its console CLI is on
  by default and the console component isn't in the build): mdns is pinned
  `~1.14.0`, what the boards run. A Tab5 solve stopped at "Missing required
  kconfig option after retry" (`ESP_HOSTED_HOST_USB_*_MANIFEST`, named in
  newer esp_hosted releases' rules as `== 1`, defined nowhere): the emu
  component's Kconfig defines them as ints, 0, for the P4 (as bools the
  manager rejects `== 1`). Changing the manifest re-solves every project's
  next build, both boards.

- **Never block interrupts on core 0 for long.** The LCD has no frame memory;
  its 10-line bounce buffers are refilled by an interrupt on core 0 with well
  under 1 ms of slack. Reading the chip temperature sensor (regi2c,
  interrupts off) on core 0 caused artefacts and a shifted picture; it's
  read on core 1, on request, at most every 5 s. A flood of log output has a
  similar effect.
- **ESP-IDF's RGB driver never got back in step** (5.5.4, still in
  5.5.6; 6.1 does half of our fix; `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`): the DMA EOF count chooses the
  bounce buffer each refill goes to, and that path never reset it, so one
  lost refill interrupt (two EOFs in one ISR) left every later refill a
  buffer out of step: the picture shifted up 10 panel lines (5-7 machine
  lines) with artefacts, until a restart. Any stall over ~1 ms did it about
  half the time (a flash write holds off the non-IRAM LCD interrupts; the
  Mac's whole-picture rewrites, the ST's temperature reads). Turning the
  option off isn't enough (it misses a lost VSYNC). Fixed in
  `components/esp_lcd`: unless a frame ends in step, the next starts afresh
  (count 0, both bounce buffers refilled), logged as "LCD output was out of
  step". Test: console `x <ms>` holds off core 0's interrupts; from 2 ms on
  each stall is put right at the next VSYNC (2026-10-17).
- **Fewer framebuffer writes, fewer artefacts**: the presenter used to
  rewrite the whole 1 MB picture whenever anything changed; with a Mac
  (cursor, pointer) that was constant PSRAM traffic against the LCD's reads,
  and the panel showed artefacts and a picture shifted by ~5 lines. It now
  hashes each source line and writes only rows that changed.
- **No `esp_partition_read()` of big blocks while the LCD runs**: it turns
  the caches off for the whole read. Memory-map partitions instead.
- **LVGL is pinned to 9.4** (`idf_component.yml`): the version both boards
  were tested with. A project that resolved 9.6 needed its `sdkconfig`
  regenerated after going back (9.6 left an empty include setting).
- **Big emulated RAM must be allocated first** at power-on; later PSRAM is
  too fragmented for 4 MB in one block.
- **Internal RAM is tight** (the ST runs with 67-100 KB free): WiFi before
  BLE, WiFi IRAM options off, few static RX buffers,
  `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, 80 KB reserved for DMA/internal, LVGL's
  heap in PSRAM (`lv_mem_psram.c`). Keep big buffers of machines in PSRAM.
- **The HTTP server handles one request at a time**: uploads and downloads
  run in tasks of their own (`httpd_req_async_handler_begin`), or a large
  disk image blocks the page and the API for minutes. Two at a time: each
  upload has its own temporary file (`<name>.tmp`; a shared `upload.tmp`
  failed the second).
- **macOS waits 5 s for an IPv6 answer** for `.local` names: the board makes
  an IPv6 link-local address so mDNS answers it.
- **SD writes from a PSRAM buffer go sector by sector** on the S3: use
  DMA-capable internal buffers for big writes (web uploads). The P4's SDMMC
  takes PSRAM buffers directly when they're aligned to the cache line.
- **The LCD screen dump streams 12 rows at a time**: a whole frame plus
  base64 (2.8 MB) doesn't fit next to a 4 MB machine.
- **Opening the serial port** with RTS=1 and DTR=0 resets the chip;
  `emu_console.py` keeps DTR high. Writes use 1 KB chunks with a timeout (a
  `portMAX_DELAY` write once hung the board).
- **Simulated touches** (`t x y`) last 150 ms: send taps in separate
  `emu_console.py send` calls, or LVGL sees one press sliding between them.
  `t x y ms x2 y2` drags (moving for 3/4 of the time, then still: no
  fling): `t 425 440 800 425 240` scrolls the Tab5's choice rows to the
  end.
- Flash writes (NVS) briefly stall both cores: write from core 0 tasks, not
  from the emulation.
- **Profile before optimising**: `emu_console.py PORT profile build/X.elf`.
  On the Commodore, two good guesses (the CPU core in IRAM, more inlining)
  gained nothing; the profile showed the real costs below.
- **Read-only data in flash is slow for hot code**: it is read through a
  data cache that the LCD's PSRAM traffic keeps flushing. A CPU core's
  opcode jump table there cost a tenth of the emulation's time (the 6502's
  `linker.lf`: `noflash_data`; the ST copies Musashi's tables to PSRAM).
- **A 32 KB instruction cache** (`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB`)
  made the Commodore's busy disk drive 25% faster, for 16 KB of internal RAM.
- **No 64-bit division in hot paths**: it is a libgcc call (the
  Commodore's drive clock conversion did one per drive instruction).
- **Tab5: esp_hosted pads every SDIO transfer to whole 512-byte blocks**, and
  ESP-IDF's SDIO block write returns `ESP_ERR_INVALID_ARG` without a word
  when the buffer isn't aligned to the cache line. Its pool is in PSRAM
  (`ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`: internal RAM is too small, it asserts
  in `sdio_mempool_create` before `app_main`) aligned to 64 bytes, so the L2
  cache line must be 64 bytes (`CACHE_L2_CACHE_LINE_64B`): with 128 every
  other packet failed ("Failed to send data: 258", "Unrecoverable host sdio
  state", a restart every ~8 s). esp32_basic runs with 128.
- **Tab5: the SD card and the C6 share the SDMMC host.** A failed mount
  deinitialises its slot, and the host with the last slot: esp_hosted has
  started the host by then but not its slot. `sdcard.c` gives the mount a
  `deinit_p` that does nothing.
- **Tab5: web transfers don't alternate between the SD card and WiFi**
  (same host: while the card writes, the C6 can't pass packets on and drops
  them). A file is received whole into PSRAM, then written (`write()` past
  stdio, a 128-byte aligned buffer: FATFS writes cluster runs from it); a
  download is read whole, then sent with a Content-Length (`WHOLE_FILES` in
  `web.c`, when it fits with 4 MB to spare; else 8 KB pieces). With the
  Tab5's TCP windows at 64 KB (board `sdkconfig.defaults`; the S3's 16 KB /
  5.7 KB stay): 4 MB up 0.28 → 1.3-1.6 MB/s, 16 MB 2 MB/s; down 0.6 →
  0.75-1.6 MB/s (the C6 sends; it reads 4 MB from the card in 0.26 s).
  32 KB pieces in between were worse (0.33 MB/s up). The MaTouch: 0.55 up,
  0.31 down, as before. The C6's own transmit settings (BA window) are in
  its firmware: esp_hosted passes only buffer counts and the receive window.
- **Tab5: power the C6 before esp_hosted connects** (BSP WiFi feature and
  PI4IOE5V6408 0x44 pin 0, ~2 s to boot); esp_hosted's constructor tries at
  boot, `esp_hosted_init()` connects later. Its Bluetooth controller is off
  until `esp_hosted_bt_controller_init/enable` (esp32_basic, radio_c6.c).
- **Tab5: WiFi credentials live on the C6** (esp_wifi_remote: the C6's NVS),
  so a network another firmware joined is joined again.
- **Tab5: the battery charges only with CHG_EN** (IO expander 0x44 pin 7)
  high, which nothing else sets (esp32_basic).
- **Tab5: the battery's state from the INA226** (0x41, M5Unified's way):
  the pack's voltage, its current through a 5 mOhm shunt that reads
  charging as negative; charging, discharging or idle by that current
  (10 mA), the level from the voltage (2S: 3.3-4.15 V a cell, high while
  charging). The IP2326's status line (0x44 pin 6) reads high whether it
  charges or not: not used. Shown at the right of the setup screen's
  WiFi and time lines (an icon filled to the level, as wmbus_beacon's
  T-Deck draws it, and charging or not) and in the web page's Board card
  (`board.battery` in the status, with the charger's lines read back from
  the expander's registers); `BOARD_HAS_BATTERY` 0 on the MaTouch.
- **Tab5: CHG_EN high wasn't enough: set the charger's other lines as
  M5Unified does.** The expander driver's reset leaves every pin a
  floating output with a pull-down; with only CHG_EN driven, the user's
  flat pack (5.45 V, 2.7 V a cell, too flat to start the board) took 3 mA:
  not charging. Driving the QC line (pin 5) low and making the status line
  (pin 6) an input without the pull-down: 50 mA at once (the trickle
  below 6 V that M5Stack's docs describe), the voltage rising ~2 mV a
  second (2026-11-15). Which of the two did it wasn't separated. The
  expander's input register reads 0 for a pin set as an output, driven or
  not: an output's state is its direction, high-Z and output bits.
- **Tab5: the RX8130CE RTC (0x32) holds UTC**, like esp32_basic (wmbus_hub
  writes local time: after it the hour is off until the internet time).
  At boot it sets the system clock (`net_time_source()` says `BOARD`, the
  setup screen shows the time before WiFi is up, ~20 s before the internet
  time comes); each internet time is written back. An unset chip reads as
  plausible garbage (`a1 00 00 11 37 37`, year 2101, VLF set): VLF and the
  date are checked; writing clears VLF.
- **Tab5: pinned versions** (`idf_component.yml`): BSP 1.3.0, esp_hosted
  2.12.9 (3.x changed its API), esp_wifi_remote ~1.6.4, esp_lvgl_port < 2.9
  (2.9 needs a newer ESP-IDF), the set esp32_basic runs.
- **Tab5: opening the port right after a flash or reset** can fail ("Device
  not configured") while it re-enumerates, and the first seconds of the boot
  log are lost: read the log again a moment later.
- **A setup screen label above fixed-position widgets must stay one line**:
  the Disks page placed the drive cards for a one-line description, the
  Mac's longer one wrapped and its second line hid under the first card.
  The label has a one-line height now (`LV_LABEL_LONG_DOT` truncates).
- **The debugger costs a pointer test per instruction** (`dbg_map`, NULL
  without a client; the 65C816's in its outer loop only, one instruction a
  turn while it has a map). On the computer the cores ran as fast with it
  as without. On the boards (2026-10-18, `CONFIG_EMUDBG` on against off,
  the same work): the C128 booting CP/M from a busy 1571 on the MaTouch
  74.6% against 74.9% of real time (within the runs' spread), the XL at
  DOS core 1 63.0% against 61.5%, the IIGS at the Finder 79.2% against
  79.1%, the ST on the Tab5 at 8 and 16 MHz and Fastest the same.
- **The debugger's server task has its stack in PSRAM** (12 KB): with 4 KB
  of internal RAM the S3 had no 16 KB block left for the supervisor in the
  Commodore's build: its creation failed without a word, and the setup
  corner and touches were dead (now logged as an error). And the first
  68000 `disasm` overflowed 4 KB (Musashi builds its table with a copy of
  its opcode list on the stack): the board rebooted. The task never
  touches the flash, which a PSRAM stack wouldn't allow.
- **WiFi modem sleep adds ~100 ms to every request** that comes after a
  pause (the radio wakes at the access point's beacons; the Tab5's C6 up
  to 650 ms): a debugger's steps felt it. While a client is connected the
  stats task turns it off (`net_low_latency()`); pings 2-3 ms instead of
  ~50-230 ms.
- **A debugger stop blocks the emulation task inside the CPU core**, between
  two instructions, so no machine's `run_frame()` had to learn to stop
  halfway. Everything that pauses the emulation (`emu_paused()`: the setup
  screen, disk changes from the web page) first lets a stopped machine go
  on to the end of its frame (`emudbg_suspend()`), or it would wait forever.
- **Steps run through interrupts and count returns by the stack**, not by
  the address after the call: calls with inline parameters (ProDOS MLI,
  the C128's PRIMM, GS/OS, the Mac's A-traps) return further on. An
  interrupt handler's start is taken from the vectors when the step
  begins (the Z80's RST $38 is also IM 1's entry: told apart by where it
  returns).
- **Musashi's disassembler has a 256 KB table**: static, it overflowed
  the S3's DRAM by 74 KB once the debugger used the disassembler (before,
  `--gc-sections` dropped it). It is allocated on first use now (PSRAM).
- **The Commodores type as a PC keyboard**: lowercase letters are the
  C64's capitals; "RUN" typed as capitals comes out shifted (graphics).

## Repositories

Public GitHub repositories, cloned side by side:
`github.com/rlucencic/esp32emu`, `esp32ste`, `esp32mac`, `esp32cpc`, `esp32zx`,
`esp32cbm`, `esp32xl`, `esp32msx`, `esp32gs`, `esp32mac2`, `esp32mac3`, `esp32coco`, `esp32arc`, `esp32pc`, `esp32ami` (public since 2026-10-21), `esp32ql` (public since 2026-11-18, in development), and `esp32emu-vscode` (the debugger's VS Code
extension, TypeScript: `npm test`, `npm run test:host` against the demo
machine, `npm run test:vscode` inside VS Code). The user pushes straight to `main`; commit messages end with the
`Co-Authored-By` line the session gives. When the user says "commit and
push all", that's all of them (esp32emu first: the others build against
it). Topics on GitHub: emulator, esp-idf, esp32-s3, lvgl, m68k (z80 for the
CPC and the Spectrum, 6502 for the Commodore and the Atari XL),
retrocomputing, makerfabs, matouch, esp32-p4, m5stack, m5stack-tab5,
risc-v, plus the machine's own (atari-st, macintosh, amstrad-cpc,
zx-spectrum, atari-8bit, ...); each README has badges and screenshots, the
About text lists the main features. esp32emu's own: the CPUs (m68k, z80,
6502), upd765, both boards' and one per machine (atari-st, macintosh,
amstrad-cpc, zx-spectrum, commodore-64, atari-8bit). esp32emu, esp32mac,
esp32cpc, esp32zx, esp32cbm and esp32pc (its topics added 2026-10-21: the boards' eleven
plus ibm-pc, x86, 8088, 80286, ms-dos, dos, vga, adlib, sound-blaster) are at GitHub's limit of 20: a new topic
replaces one (for the Tab5: embedded, floppy-controller, musashi, ble-hid,
floppy-disk, tape-emulation, cp-m, vic-ii and 1571 went; esp32emu's risc-v
for atari-8bit, esp32-p4 says it). Its About text names the runtime's parts
and the cores.

## README screenshots

- The machine's screen: `emu_console.py PORT screenshot F.png` (its own
  resolution); the whole picture: `lcd F.png` (1024x600, the Tab5 1280x720:
  the landscape picture, not the panel's own framebuffer).
- The setup screen: hold the corner with `send "t 1010 12 2600"` (Tab5:
  `t 1265 15 2600`; pauses the machine), tap buttons with `t x y`, capture
  with `lcd`. Its status lines show the WiFi name, IP and time: cut them out
  (PIL in a scratch venv, or ImageMagick: `magick in.png \( -clone 0 -crop
  1280x124+0+0 \) \( -clone 0 -crop 1280x506+0+214 \) -delete 0 -append
  +repage out.png`). MaTouch, up to two choice rows: rows 104-190
  (1024x514, the first buttons at y=112); three rows (closer): 104-180
  (1024x524); four rows (the old squeezed layout): 104-170 (1024x534);
  more rows now scroll (not measured on the MaTouch yet). Tab5, three
  rows: 124-214 (1280x630). Taken before the
  machine starts, it says "Start" (while it's paused "Back").
- This repo's own: `docs/images/matouch-setup-xl.png` (1024x524) and
  `tab5-setup-xl.png` (1280x630), the Atari XL's setup screen on each board
  with the new button labels (shown 49% / 50.9%: the same height). The
  ST's older ones (`matouch-setup-st.png`, `tab5-setup.png`, "Start
  Atari") are no longer shown.
- The other projects' setup screenshots (esp32ste, esp32mac, esp32cpc,
  esp32zx, esp32cbm) still show "Start Atari", "Start Mac", ...: retake
  them (new files) when that machine is on the board again.
- The web page: never the live board's page (WiFi name, IP, SD card).
  Serve the real `web/index.html` (and the machine's `/machine.js`) from a
  small local node server that answers `/api/info` (copied from the
  board), `/api/status` and `/api/disks` with example data (HomeNetwork,
  192.168.1.42, Keyboard/Mouse, made-up disk names) and saves `PUT
  /disks/...`; open it in Chrome (devtools: dark scheme, 1185 px wide),
  drop a file to show the log, full-page screenshot. Chrome's file upload
  only takes files inside the project folders (stage them in `build/`).
- Never overwrite an existing screenshot: a new file name each time (the
  user asks for "another screenshot" to add one, not to replace one).

## To do

- **IN PROGRESS — Sinclair QL (1984) emulator (`esp32ql`)** (started
  2026-11-18: both boards, the user's call; its TODO.md has the steps)

  Develop for ESP32-S3 alongside `rpi5ql` (Raspberry Pi 5). Use the existing
  `emu_machine_t` interface for LCD display, embedded audio, and keyboard
  input. Keep the QL machine core portable so both projects can share it
  and its compatibility tests.

  - Motorola 68008 at 7.5 MHz, with the shared Musashi core as the starting
    point for the instruction set and QL-specific timing for the 8-bit bus.
  - 128 KB RAM, 48 KB ROM, and ZX8301 video and memory control. Support
    512×256 four-colour and 256×256 eight-colour display modes.
  - ZX8302 peripheral control, Intel 8049 keyboard and sound behaviour,
    and both built-in Microdrives with cartridge image support.
  - Boot QDOS into SuperBASIC and test original software, including loading
    and saving a document in Psion Quill.

- **Acorn Electron (1983) emulator (`esp32electron`)** follows the Sinclair QL.

  Develop for ESP32-S3 alongside `rpi5electron` (Raspberry Pi 5). Use the
  shared `m6502` core and the existing `emu_machine_t` interface for LCD
  display, embedded audio, and keyboard input. Keep the machine core
  portable so both projects can share it and its compatibility tests.

  - 6502A with ULA-controlled timing, including 2 MHz ROM accesses, 1 MHz
    RAM accesses, and video contention in modes 0–3.
  - 32 KB RAM, 32 KB ROM, Acorn MOS, and BBC BASIC.
  - Screen modes 0–6, single-channel sound, and keyboard emulation.
  - Cassette loading from UEF images and compatibility tests with original
    Electron software. Consider Plus 1 and Plus 3 support after the base
    machine.

- **Oric Atmos (1984) emulator (`esp32oric`)** follows the Acorn Electron.

  Develop alongside `rpi5oric` (Raspberry Pi 5) using the existing ESP-IDF
  sibling-project layout. Use the shared `m6502` core and integrate with
  `esp32emu` through `emu_machine_t`. Optimize for ESP32-S3 with PSRAM-aware
  memory management and use the board runtime for LCD display, embedded
  audio, and keyboard input.
  Share emulator design concepts, compatibility tests, and technical
  documentation with `rpi5oric` where practical.

  - MOS 6502A CPU at 1 MHz, 48 KB RAM, 16 KB ROM, and memory banking.
  - Oric ULA with accurate video attributes, 40×28 text, and 240×200
    high-resolution graphics.
  - AY-3-8912 three-channel sound and MOS 6522 VIA with timers and interrupts.
  - Oric keyboard emulation and cassette support for TAP files. Microdisc
    floppy disk emulation is optional.
  - Compatibility with Oric Atmos BASIC 1.1 and testing with original Oric
    software.

- **README screenshots, from time to time** (the user, 2026-10-21): for
  every emulator, replace them with more spectacular ones as we find them
  (games, demos, desktops at their best), following "README screenshots"
  above (new file names, no private data). Known stale: the siblings'
  `web.png` shows the page before the free RAM and disk space bars
  (esp32ami's is current).
- **Assembler for time-critical code on both boards** (the user,
  2026-10-21): RISC-V on the ESP32-P4 (an open ISA) and Xtensa LX7 on the
  ESP32-S3 (the user checked: it may be programmed in assembler; ESP-IDF
  and esp-dsp contain Xtensa assembler themselves). Wherever C code can be
  made faster with assembler, do it. The P4 build compiles with
  `-march=rv32imafc_zicsr_zifencei_xesppie`, so `.S` files can also use
  Espressif's PIE (128-bit SIMD on 8/16/32-bit lanes, `esp.*` mnemonics,
  documented in the P4's Technical Reference Manual; esp-dsp is written
  that way); the S3 has its own PIE (`ee.*`, 128-bit, in the S3's TRM).
  The MaTouch gains most: it is the board short of time (the ST ~78% of
  real time on average, no 16 MHz). Each `.S` per CPU, with the C kept
  for the host builds. Profile first
  (`emu_console.py PORT profile ELF`; it samples core 1 only, the
  presenter is on core 0), then memory placement (TCM, internal RAM) and
  the P4's own hardware (the PPA could scale as well as rotate, 2D-DMA)
  before assembler. Candidates: planar-to-chunky drawing (the Amiga's
  Denise, the ST's bitplanes), line conversion and scaling, big blits,
  the CPU cores' dispatch and memory fast paths.
- **The Tab5 Keyboard's two LEDs as drive lights** (the user,
  2026-10-21): it has two programmable RGB LEDs; light one for floppy and
  one for hard disk activity (the drives' `light` the web page shows), in
  every emulator. No power LED: on the board that no longer means
  anything. `tab5_keyboard.c` doesn't touch them yet; esp32_basic's
  `main/tab5_keyboard.c` does (an SD activity light): REG_MODE_RGB $11 = 1
  (custom), RGB1 at $60-$62 and RGB2 at $64-$66 (B, G, R), brightness $03
  (0-100); the I2C bus is shared with the key polling, so write only on a
  change, from the keyboard's own task.
- **The machines' serial ports over USB** (the user, 2026-10-21): a
  machine's serial port (the ST's MFP RS-232, the PC's COM ports, the
  Amiga's Paula, ... wherever there is one) as a USB serial port for the
  computer at the other end of the cable. The Tab5 has one USB-C port: it
  powers the board and would carry the serial port too. The MaTouch has a
  second USB port: "the first one must be for powering the device, and
  the second one will be used to emulate the serial port". To settle: which
  of the MaTouch's ports is which (the console is on the S3's own USB
  today, `/dev/cu.usbmodem1101`), and where the console goes when a port
  is the machine's (on the Tab5 it is the only one).
- **Clock and time zone** (an audit of every machine's clock, 2026-10-21):
  the clock chips (ST IKBD, Mac RTC/Egret, IIGS, Archimedes PCF8583, Amiga
  MSM6242, PC MC146818, MSX RP5C01, CPC Symbiface II) all read `time()` +
  `localtime_r()` on each access: the Tab5's RTC chip sets the system time
  at boot (before the machine starts), NTP on both boards (written back to
  the Tab5's RTC), so a later sync shows at once. To fix here: the time
  zone is set only in `net_init()` (`net.c`), so with WiFi off (or the
  Tab5's C6 not starting) local time is UTC, and the Tab5 shows its RTC's
  UTC as local: set TZ in `emu_run()` before `tab5_init()`. No shared
  "not set yet" (the MaTouch before NTP is at 1970; a manual Start there
  starts the machine then, autostart waits): the Archimedes and the PC's
  BIOS take years before 2024 as unset, the MSX shows garbage, the PC's
  CMOS 1970 with a good battery; perhaps one `emu_time_set()` for all.
  Guests that read the clock at boot and then count ticks (GEMDOS, DOS,
  the Mac) fall behind below real time. Each machine's own findings are
  in its CLAUDE.md's open items.

## Working with the user

- Expects changes built (all projects), flashed and checked on the board,
  then a short report of what was verified and what wasn't. Flashing ends
  the session on the board: say so first.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing.

## Display regression checks (2026-11-08)

`docs/display-reliability-2026-11-08.md` records a JTAG-confirmed Tab5 PPA
completion stall and an independently reproduced presenter snapshot/hash
race. Flushes now expand to eight-row boundaries; hashes describe the
source snapshot actually scaled. Run `python3 tools/test_tab5_flush.py`
and `python3 tools/test_presenter_snapshots.py` for ASan/UBSan checks of
the actual functions. All 24 project/board builds pass. Fresh MSX sprite
baseline/candidate measurements use both fixes; old timings across the
runtime change are not directly comparable. S3 runtime remains untested.
