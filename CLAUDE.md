# esp32emu: the shared board runtime for the 68000 emulators

ESP-IDF components used by `../esp32ste` (Atari STE) and `../esp32mac`
(Macintosh Plus/SE/Classic) on the Makerfabs MaTouch ESP32-S3 7". It is a
library: it isn't built on its own. Build and test through one of the
projects (each has `EXTRA_COMPONENT_DIRS` pointing here). ESP-IDF v5.5.4, C.

Changes here affect both emulators: build both after any change.

## Layout

| Path | What |
|---|---|
| `components/musashi/` | 68000 core (Musashi, evansm7 fork) + `m68kconf.h` with the `m68k_host_*()` hooks; `test/` host tests (`make -C components/musashi/test`) |
| `components/emu/include/emu/emu.h` | The machine interface (`emu_machine_t`) and runtime API |
| `components/emu/emu.c` | Boot order, emulation task (core 1), presenter, stats, temperature, supervisor (touch: setup corner and machine touches), pausing |
| `components/emu/setup_ui.c` | LVGL setup screen (status, WiFi, password, Bluetooth, pairing, drives, images) |
| `components/emu/web.c` + `web/index.html` | Web server and the page (one page for all machines: it reads `/api/info`) |
| `components/emu/disks.c` | Disk image folder, drives, NVS memory of inserted disks, blank disks |
| `components/emu/console.c` | Serial console (`include/emu/console.h` lists the commands) |
| `components/emu/ble_hid.c` | BLE HID host: pairing, reconnecting, reports -> HID usages |
| `components/emu/net.c` | WiFi station + SNTP |
| `components/emu/lcd.c`, `touch.c`, `sdcard.c`, `speaker.c` | Board drivers; pins in `include/emu/board.h` |
| `components/emu/json.c` | Small JSON builder for the web API |
| `tools/emu_console.py` | Console client (run with the IDF venv's Python, `-I`) |

## How a machine plugs in

- `emu_run(&machine)` from `app_main`. Order: NVS, `reserve_memory()` (the
  machine's big PSRAM blocks first), SD card -> `storage(sd)`, disks restored,
  LCD, touch, console, WiFi, web, BLE, stats/temperature tasks, setup screen,
  `start()`; the setup screen comes back with `emu_set_message()` text when
  `start()` fails (e.g. no ROM).
- `start()` may be called again (setup screen choices changed, console):
  always with the emulation paused. If it can't get its memory any more,
  `emu_reboot_into_machine()` restarts the board straight into the machine.
- Setup screen choices: `get`/`set` (set saves). Changing one while running
  turns "Back to X" into "Restart X"; if the restart fails, the runtime sets
  the old choices back and starts again.
- Inputs arrive from other tasks: `hid_key(usage, pressed)` (modifiers as
  usages $E0-$E7, each side separately), `hid_mouse(dx, dy, buttons)`,
  `touch(x, y, pressed)` in machine screen pixels (only if the machine sets
  it; the top-right setup corner is never forwarded).
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
  `view_width` x 600 (centred) when `screen_signature()` changes;
  `EMU_SCALE_SMOOTH` filters (1-bit pictures at odd factors), `NEAREST` doesn't.

## Lessons (from the STE, where most of this code was written)

- **Never block interrupts on core 0 for long.** The LCD has no frame memory;
  its 10-line bounce buffers are refilled by an interrupt on core 0 with well
  under 1 ms of slack. Reading the chip temperature sensor (regi2c,
  interrupts off) on core 0 caused artefacts and a shifted picture; it's
  read on core 1, on request, at most every 5 s. A flood of log output has a
  similar effect.
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
- **A setup screen label above fixed-position widgets must stay one line**:
  the Disks page placed the drive cards for a one-line description, the
  Mac's longer one wrapped and its second line hid under the first card.
  The label has a one-line height now (`LV_LABEL_LONG_DOT` truncates).

## Repositories

Three public GitHub repositories, cloned side by side:
`github.com/rlucencic/esp32emu`, `esp32ste`, `esp32mac`. The user pushes
straight to `main`; commit messages end with the `Co-Authored-By` line the
session gives. When the user says "commit and push all", that's all three
(esp32emu first: the others build against it). Topics on GitHub: emulator,
esp-idf, esp32-s3, lvgl, m68k, retrocomputing, makerfabs, matouch, plus
the machine's own (atari-st, macintosh, ...); each README has badges and
screenshots, the About text lists the main features.

## README screenshots

- The machine's screen: `emu_console.py PORT screenshot F.png` (its own
  resolution); the whole panel: `lcd F.png` (1024x600).
- The setup screen: hold the corner with `send "t 1010 12 2600"` (pauses
  the machine), tap buttons with `t x y`, capture with `lcd`. Its status
  lines show the WiFi name, IP and time: cut rows 104-190 out (PIL, in a
  scratch venv), which leaves 1024x514 with the first buttons at y=112,
  like the ST's image. Taken before the machine starts, it says "Start X".
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

- Expects changes built (both projects), flashed and checked on the board,
  then a short report of what was verified and what wasn't. Flashing ends
  the session on the board: say so first.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing.
