# esp32emu: the shared board runtime for the emulators

ESP-IDF components used by `../esp32ste` (Atari STE), `../esp32mac`
(Macintosh Plus/SE/Classic), `../esp32cpc` (Amstrad CPC and Plus),
`../esp32zx` (ZX Spectrum 128K, +2, +3) and `../esp32cbm` (Commodore 64 and
128) on two boards: the Makerfabs MaTouch ESP32-S3 7" (target esp32s3, the
default) and the M5Stack Tab5 (target esp32p4). It is a library: it isn't
built on its own. Build and test through one of the projects (each has
`EXTRA_COMPONENT_DIRS` pointing here). ESP-IDF v5.5.4, C. The 68000
machines use Musashi, the Z80 ones the Z80 core, the Commodores the 6502
core (the C128 the Z80 too); the CPC and the +3 share the uPD765.

Changes here affect all five emulators on both boards: build all ten after
any change (only the components a project requires get built: MINIMAL_BUILD).

## The two boards

- **MaTouch** (`/dev/cu.usbmodem1101`): `idf.py build`, `sdkconfig`, `build/`.
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
  becomes `<name>-2.local` (`/api/info` says which board: `"board"`).
- The board code: `include/emu/board.h` picks `board_matouch7.h` or
  `board_tab5.h`; `lcd.c` (RGB panel) or `lcd_tab5.c` (MIPI-DSI through the
  BSP); `touch.c`, `sdcard.c`, `speaker.c` have both; `tab5.c` (charging, the
  C6, esp_hosted) and `tab5_keyboard.c` only build for the Tab5.
- `BOARD_LCD_H_RES` x `BOARD_LCD_V_RES` is the landscape picture everything
  draws (presenter, setup screen, `L` dumps, touches): 1024x600 or 1280x720.
  On the Tab5 `lcd_flush(y0, y1)` has the PPA turn those rows onto the
  720x1280 portrait panel; landscape (x, y) is panel (y, 1279 - x), the
  orientation esp32_basic uses with the Tab5 Keyboard below the screen. A
  self-test at start checks it ("PPA rotation is not the expected one").
- The setup screen is laid out for 1024x600: `SX()`/`SY()` scale it (exact
  on the MaTouch), `FONT_BIG` is 34 px on the Tab5, the default font 28.
  Machines' `view_width` is for 600 lines: `emu_view_width()` scales it.

## Layout

| Path | What |
|---|---|
| `components/musashi/` | 68000 core (Musashi, evansm7 fork) + `m68kconf.h` with the `m68k_host_*()` hooks; `test/` host tests (`make -C components/musashi/test`) |
| `components/z80/` | Z80 core (from the CPC): T-states with optional ZX contention, or CPC microseconds (menuconfig "Z80 core"); `Z80_READ_SPECIAL` sends reads of NULL banks to the bus (the C128's MMU); `test/` zexdoc/zexall host test for each build |
| `components/m6502/` | 6502 core (from the Commodore): every bus cycle, all 256 opcodes, several CPUs at once, `m6502_run()` to the machine's next event; `linker.lf` keeps its jump table in internal RAM; `test/` Klaus Dormann's functional test and an opcode cycle table (`make -C components/m6502/test`) |
| `components/upd765/` | uPD765A floppy controller with DSK/EDSK images (CPC, +3) |
| `components/emu/include/emu/emu.h` | The machine interface (`emu_machine_t`) and runtime API |
| `components/emu/emu.c` | Boot order, emulation task (core 1), presenter, stats, temperature, supervisor (touch: setup corner and machine touches), pausing |
| `components/emu/setup_ui.c` | LVGL setup screen (status, WiFi, password, Bluetooth, pairing, drives, images) |
| `components/emu/web.c` + `web/index.html` | Web server and the page (one page for all machines: it reads `/api/info`) |
| `components/emu/disks.c` | Disk image folder, drives, NVS memory of inserted disks, blank disks |
| `components/emu/console.c` | Serial console (`include/emu/console.h` lists the commands), with `p`: a 20 kHz sampling profiler of core 1 |
| `components/emu/ble_hid.c` | BLE HID host: pairing, reconnecting, reports -> HID usages |
| `components/emu/net.c` | WiFi station + SNTP |
| `components/emu/lcd.c`, `lcd_tab5.c`, `touch.c`, `sdcard.c`, `speaker.c` | Board drivers; `include/emu/board.h` picks the board's header |
| `components/emu/tab5.c`, `tab5_keyboard.c` | Tab5: battery charging, the ESP32-C6 (power, esp_hosted, its Bluetooth controller), the Tab5 Keyboard |
| `components/fatfs/` | ESP-IDF 5.5.4's fatfs with `FF_FS_EXFAT` 1 (from esp32_basic): exFAT SD cards. Overrides ESP-IDF's for every project: update it with ESP-IDF |
| `components/esp_lcd/` | ESP-IDF 5.5.4's esp_lcd (without test_apps), one change marked `esp32emu` in `rgb/esp_lcd_panel_rgb.c` (+ `emu_lcd_rgb_resyncs` in its header): see the lessons. Overrides ESP-IDF's for every project and both boards (a `reconfigure` picks it up): update it with ESP-IDF |
| `boards/m5stack_tab5/sdkconfig.defaults` | The Tab5's settings (PSRAM, esp_hosted SDIO pins, NimBLE over VHCI, fonts, speaker on) |
| `components/emu/json.c` | Small JSON builder for the web API |
| `tools/emu_console.py` | Console client (run with the IDF venv's Python, `-I`); `profile ELF [N [TOP]]` runs `p` and names the hottest functions |

## How a machine plugs in

- `emu_run(&machine)` from `app_main`. Order: NVS, `reserve_memory()` (the
  machine's big PSRAM blocks first), (Tab5: charging, C6 power), SD card ->
  `storage(sd)`, disks restored, LCD, touch, console, (Tab5: esp_hosted),
  WiFi, web, (Tab5: the C6's Bluetooth controller), BLE, (Tab5 Keyboard),
  stats/temperature tasks, setup screen, `start()`; the setup screen comes back with `emu_set_message()` text when
  `start()` fails (e.g. no ROM).
- `start()` may be called again (setup screen choices changed, console):
  always with the emulation paused. If it can't get its memory any more,
  `emu_reboot_into_machine()` restarts the board straight into the machine.
- Setup screen choices: `get`/`set` (set saves). Changing one while running
  turns "Back to X" into "Restart X"; if the restart fails, the runtime sets
  the old choices back and starts again. A `live` choice (the ST's CPU
  speed, Tab5 only) changes the machine as it's set, no restart; `set()` may run on
  another task, so the machine picks the value up between frames. Three
  choice rows fit closer (`tight` in setup_ui.c).
- Inputs arrive from other tasks: `hid_key(usage, pressed)` (modifiers as
  usages $E0-$E7, each side separately), `hid_mouse(dx, dy, buttons)`,
  `touch(x, y, pressed)` in machine screen pixels (only if the machine sets
  it; the top-right setup corner is never forwarded).
- Drives of different kinds (the CPC's floppies and cartridge slot): the
  optional `disks_config_t.accepts(drive, name)` limits the setup screen's
  image list for a drive, `takes_blank(drive)` its "New" button. The web
  page offers every drive; `insert()` refuses what doesn't fit.
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
- Status: `status(j, EMU_STATUS_ROWS)` adds `emu_json_row()`s for the
  machine card, `status(j, EMU_STATUS_DRIVES)` adds `emu_json_drive()`s.
- Presenter: `render_line()` at the machine's resolution, scaled to
  `emu_view_width()` x the panel's height (centred) when
  `screen_signature()` changes, then `lcd_flush()` of the rows it wrote;
  `EMU_SCALE_SMOOTH` filters (1-bit pictures at odd factors), `NEAREST` doesn't.

## Lessons (from the STE, where most of this code was written)

- **Never block interrupts on core 0 for long.** The LCD has no frame memory;
  its 10-line bounce buffers are refilled by an interrupt on core 0 with well
  under 1 ms of slack. Reading the chip temperature sensor (regi2c,
  interrupts off) on core 0 caused artefacts and a shifted picture; it's
  read on core 1, on request, at most every 5 s. A flood of log output has a
  similar effect.
- **ESP-IDF's RGB driver never got back in step** (5.5.4,
  `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`): the DMA EOF count chooses the
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
  disk image blocks the page and the API for minutes.
- **macOS waits 5 s for an IPv6 answer** for `.local` names: the board makes
  an IPv6 link-local address so mDNS answers it.
- **SD writes from a PSRAM buffer go sector by sector**: use DMA-capable
  internal buffers for big writes (web uploads).
- **The LCD screen dump streams 12 rows at a time**: a whole frame plus
  base64 (2.8 MB) doesn't fit next to a 4 MB machine.
- **Opening the serial port** with RTS=1 and DTR=0 resets the chip;
  `emu_console.py` keeps DTR high. Writes use 1 KB chunks with a timeout (a
  `portMAX_DELAY` write once hung the board).
- **Simulated touches** (`t x y`) last 150 ms: send taps in separate
  `emu_console.py send` calls, or LVGL sees one press sliding between them.
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
- **Tab5: power the C6 before esp_hosted connects** (BSP WiFi feature and
  PI4IOE5V6408 0x44 pin 0, ~2 s to boot); esp_hosted's constructor tries at
  boot, `esp_hosted_init()` connects later. Its Bluetooth controller is off
  until `esp_hosted_bt_controller_init/enable` (esp32_basic, radio_c6.c).
- **Tab5: WiFi credentials live on the C6** (esp_wifi_remote: the C6's NVS),
  so a network another firmware joined is joined again.
- **Tab5: the battery charges only with CHG_EN** (IO expander 0x44 pin 7)
  high, which nothing else sets (esp32_basic).
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

## Repositories

Public GitHub repositories, cloned side by side:
`github.com/rlucencic/esp32emu`, `esp32ste`, `esp32mac`, `esp32cpc`, `esp32zx`,
`esp32cbm`. The
user pushes straight to `main`; commit messages end with the
`Co-Authored-By` line the session gives. When the user says "commit and
push all", that's all of them (esp32emu first: the others build against
it). Topics on GitHub: emulator, esp-idf, esp32-s3, lvgl, m68k (z80 for the
CPC and the Spectrum, 6502 for the Commodore), retrocomputing, makerfabs,
matouch, esp32-p4, m5stack, m5stack-tab5, risc-v, plus the machine's own
(atari-st, macintosh, amstrad-cpc, zx-spectrum, ...); each README has badges
and screenshots, the About text lists the main features. esp32emu's own:
the CPUs (m68k, z80, 6502), upd765, both boards' and one per machine
(atari-st, macintosh, amstrad-cpc, zx-spectrum, commodore-64). esp32emu,
esp32mac, esp32cpc, esp32zx and esp32cbm are at GitHub's limit of 20: a new
topic replaces one (for the Tab5: embedded, floppy-controller, musashi,
ble-hid, floppy-disk, tape-emulation, cp-m, vic-ii and 1571 went). Its About
text names the runtime's parts and the cores.

## README screenshots

- The machine's screen: `emu_console.py PORT screenshot F.png` (its own
  resolution); the whole picture: `lcd F.png` (1024x600, the Tab5 1280x720:
  the landscape picture, not the panel's own framebuffer).
- The setup screen: hold the corner with `send "t 1010 12 2600"` (Tab5:
  `t 1265 15 2600`; pauses the machine), tap buttons with `t x y`, capture
  with `lcd`. Its status lines show the WiFi name, IP and time: cut them out
  (PIL, in a scratch venv). MaTouch, up to two choice rows: rows 104-190
  (1024x514, the first buttons at y=112); three rows (closer): 104-180
  (1024x524). Tab5, three rows: 124-214 (1280x630). Taken before the
  machine starts, it says "Start X".
- This repo's own: `docs/images/matouch-setup-st.png` (two rows: the ST on
  the MaTouch has no CPU choice), `tab5-setup.png` (three rows) — the ST's
  setup screen on each board. `matouch-setup.png` (the MaTouch with a CPU
  row) is gone.
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

## Working with the user

- Expects changes built (all projects), flashed and checked on the board,
  then a short report of what was verified and what wasn't. Flashing ends
  the session on the board: say so first.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing.
