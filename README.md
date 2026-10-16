<h1 align="center">esp32emu</h1>

<p align="center">
  <b>The shared board runtime for emulators on the Makerfabs MaTouch ESP32-S3 7".</b><br>
  Used by <a href="https://github.com/rlucencic/esp32ste">esp32ste</a> (Atari STE), <a href="https://github.com/rlucencic/esp32mac">esp32mac</a> (Macintosh Plus, SE, Classic) and <a href="https://github.com/rlucencic/esp32cpc">esp32cpc</a> (Amstrad CPC).
</p>

esp32emu is everything an emulator on this board needs apart from the machine
itself: the display and touch panel, the SD card, WiFi with the clock from the
internet, Bluetooth keyboards and mice, a setup screen, a web page for disk
images, a serial console for testing, and a 68000 core for the machines that
need one. A machine describes itself in one struct, and `emu_run()` does the
rest.

## Components

| Component | What |
|---|---|
| `components/musashi` | The 68000 core: [Musashi](https://github.com/kstenerud/Musashi), evansm7's embedded fork, with static opcode tables (see its README) |
| `components/emu` | The runtime: tasks and pacing, LCD presenter, touch, SD card, WiFi + NTP, BLE HID host, LVGL setup screen, web server, disk image manager, serial console |
| `tools/emu_console.py` | Console client: log, screenshots of the machine or the whole LCD, commands |

## Using it

In the project's top-level `CMakeLists.txt`, before `project()`:

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32emu/components")
```

Then `main` requires `emu`, and a 68000 machine's core requires `musashi`
(the CPC brings its own Z80). The machine fills in an `emu_machine_t`
(`components/emu/include/emu/emu.h`):

```c
static const emu_machine_t machine = {
    .name = "Macintosh", .short_name = "Mac",
    .view_width = 898, .scale = EMU_SCALE_SMOOTH,     /* picture on the 1024x600 panel */
    .reserve_memory = reserve_memory,                 /* big blocks first */
    .start = start, .reset = mac_reset, .run_frame = mac_run_frame,
    .screen_mode = screen_mode, .screen_signature = mac_screen_signature,
    .render_line = mac_render_line_rgb565,
    .hid_key = hid_key, .hid_mouse = hid_mouse,       /* Bluetooth */
    .touch = touch,                                   /* optional */
    .choices = choices, .choice_count = 2,            /* setup screen rows */
    .disks = &disks,                                  /* drives and image folder */
    .status = status,                                 /* web page rows and drives */
    .web_script = macfiles_js_start,                  /* optional: converters for the page */
    .web_script_end = macfiles_js_end,
    ...
};

void app_main(void)
{
    emu_run(&machine);
}
```

`emu_run()` brings up the board, shows the setup screen, starts the machine,
and runs it on core 1, one frame at a time, paced to its refresh rate (or to
the speaker). Everything else runs on core 0.

| | |
|---|---|
| Setup screen | WiFi networks and password, Bluetooth pairing, the machine's choices (e.g. system and memory), its drives. Before the first start, and any time by holding the top-right corner of the screen for 2 s; changing a choice there restarts the machine |
| Disks | One folder of images on the SD card for the machine's drives; inserted disks are remembered in NVS. Blank disks of the sizes the machine offers |
| Web page | `http://<hostname>.local`: machine, drives, board and Bluetooth status live; upload (drag and drop, ZIP files are unpacked in the browser), insert, eject, download and delete images, make blank disks (a machine can add a script that turns other files into images in the browser, e.g. the Mac's StuffIt archives) |
| Console | Native USB port (or the UART with the speaker on): screenshots, simulated touches, disk changes, reset, plus the machine's own commands |
| Bluetooth | BLE keyboard and mouse (NimBLE central), paired on the setup screen, reconnecting by themselves; keys arrive as HID usages |

## Web API

| Request | |
|---|---|
| `GET /api/info` | Machine name, drives, image kinds, blank disk sizes |
| `GET /machine.js` | The machine's script for the page (`web_script`), if it has one |
| `GET /api/status` | Machine (state, speed, the machine's rows), drives, board, Bluetooth |
| `GET /api/disks` | Images in the folder: name, size, drive |
| `PUT /disks/<name>` | Upload (body = the image); also `POST` |
| `GET /disks/<name>` / `DELETE /disks/<name>` | Download / delete (not while in a drive) |
| `PUT /api/drive/<n>` | Insert the image named in the body into drive n (1, 2, ...) |
| `DELETE /api/drive/<n>` | Eject drive n |
| `POST /api/new?size=<bytes>` | Make a blank disk image |

`curl -T game.st http://atari-ste.local/disks/` uploads from a script.
`/floppies/` works as `/disks/` for older scripts.

## Configuration

`idf.py menuconfig` → **Emulator board** (each project sets its own defaults,
such as the network name):

| Option | Default | |
|---|---|---|
| WiFi | on | Stays connected while the machine runs; the clock comes from the internet |
| Network name | `esp32emu` | `http://<name>.local` |
| Time zone | Central Europe | POSIX TZ string |
| Web page | on | |
| Setup screen | on | Optionally starting the machine by itself after a countdown |
| BLE keyboard and mouse | on | Mouse speed divider |
| Sound on the speaker | off | Volume; moves the console to the UART port |

## Hardware

Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7" (ESP32-S3-WROOM-1-N16R8:
16 MB flash, 8 MB octal PSRAM), 1024×600 RGB panel, GT911 touch, microSD,
MAX98357A amplifier. Pins: `components/emu/include/emu/board.h`.

## Licence

[MIT](LICENSE). Musashi is MIT licensed too. LVGL (MIT), NimBLE and ESP-IDF
(Apache 2.0) are fetched at build time and keep their own licences.
