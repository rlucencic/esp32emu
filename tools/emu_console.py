#!/usr/bin/env python3
"""Talk to the emulator's serial console (see components/emu/include/emu/console.h).

  emu_console.py PORT log SECONDS          print the log for a while
  emu_console.py PORT screenshot OUT.png   save the machine's screen (needs macOS sips)
  emu_console.py PORT lcd OUT.png          save the whole LCD (setup screen too)
  emu_console.py PORT send "r" ...         send console commands
  emu_console.py PORT info                 print the machine state
  emu_console.py PORT reset                hard-reset the board, then log 10 s
  emu_console.py PORT profile ELF [N [TOP [CORE]]]  where a core spends its time
                                           (CORE 1, the emulation, by default; 0 the
                                           presenter, WiFi, ...): N samples at 20 kHz
                                           (default 8000, up to 32000), the TOP
                                           functions (default 25), by the build's ELF
                                           and the chip's ROM ELF (its toolchain's nm)
"""
import base64
import bisect
import collections
import glob
import os
import re
import subprocess
import sys
import tempfile
import time

import serial


BASE64_LINE = re.compile(rb"(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{4}|[A-Za-z0-9+/]{3}=|[A-Za-z0-9+/]{2}==)")


def is_native_usb(name):
    return "usbmodem" in name


def open_port(name):
    port = serial.Serial()
    port.port = name
    port.timeout = 0.2
    if is_native_usb(name):
        # USB-Serial-JTAG resets the chip when RTS=1 and DTR=0. The OS raises
        # both on open and pyserial then applies DTR before RTS, so keep DTR
        # high to avoid passing through the reset state.
        port.baudrate = 115200
        port.dtr = True
        port.rts = False
    else:
        # USB-to-UART (console moved there for the speaker, 921600 baud):
        # keep both lines as the OS left them, so the auto-reset circuit
        # (DTR -> IO0, RTS -> EN) stays idle.
        port.baudrate = 921600
        port.dtr = True
        port.rts = True
    port.open()
    return port


def log(port, seconds):
    end = time.time() + seconds
    while time.time() < end:
        data = port.read(4096)
        if data:
            sys.stdout.write(data.decode(errors="replace"))
            sys.stdout.flush()


def screenshot(port, out_png, command=b"s"):
    port.reset_input_buffer()
    port.write(command + b"\n")
    # Collect everything first: the device drops data if we read too slowly
    data = bytearray()
    end = time.time() + 60
    while time.time() < end and b"EMUSCREEN END" not in data[-4096:]:
        data += port.read(65536)
    if b"EMUSCREEN END" not in data:
        sys.exit("timed out waiting for the screen dump")

    width = height = None
    repeat = 1
    chunks = []
    for line in bytes(data).split(b"\n"):
        line = line.strip()
        if line.startswith(b"EMUSCREEN END"):
            break
        if line.startswith(b"EMUSCREEN "):
            fields = line.split()[1:]
            width, height = int(fields[0]), int(fields[1])
            repeat = int(fields[2]) if len(fields) > 2 else 1
            chunks = []
        elif width is not None and BASE64_LINE.fullmatch(line):
            # Log lines from other tasks may land in between: skip them
            chunks.append(base64.b64decode(line))
    pixels = b"".join(chunks)
    if len(pixels) != width * height * 2:
        sys.exit(f"screen dump incomplete: {len(pixels)} of {width * height * 2} bytes")
    write_png(width, height, pixels, out_png, repeat)


def write_png(width, height, rgb565, out_png, repeat=1):
    rgb = bytearray()
    for i in range(0, len(rgb565), 2):
        p = (rgb565[i] << 8) | rgb565[i + 1]
        r, g, b = p >> 11, (p >> 5) & 0x3F, p & 0x1F
        rgb += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    # Rows repeated for pixels taller than wide (e.g. ST medium resolution)
    rows = [rgb[y * width * 3:(y + 1) * width * 3] for y in range(height)]
    rows = [r for row in rows for r in [row] * repeat]
    with tempfile.NamedTemporaryFile(suffix=".ppm", delete=False) as f:
        f.write(b"P6\n%d %d\n255\n" % (width, len(rows)))
        f.write(b"".join(rows))
        ppm = f.name
    subprocess.run(["sips", "-s", "format", "png", ppm, "--out", out_png],
                   check=True, stdout=subprocess.DEVNULL)
    os.unlink(ppm)
    print(f"{width}x{height} -> {out_png}")


def find_nm(elf):
    """The ESP-IDF toolchain's nm for the ELF's CPU (on PATH, or under ~/.espressif):
    Xtensa for the ESP32-S3 (MaTouch), RISC-V for the ESP32-P4 (Tab5)"""
    with open(elf, "rb") as f:
        machine = int.from_bytes(f.read(20)[18:20], "little")
    if machine == 243:      # EM_RISCV
        name, pattern = "riscv32-esp-elf-nm", "tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-nm"
    else:
        name, pattern = "xtensa-esp32s3-elf-nm", "tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm"
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, name)
        if os.access(p, os.X_OK):
            return p
    tools = os.environ.get("IDF_TOOLS_PATH", os.path.expanduser("~/.espressif"))
    found = sorted(glob.glob(os.path.join(tools, pattern)), key=os.path.getmtime)   # newest last
    if not found:
        sys.exit(f"no {name}: run with the ESP-IDF environment")
    return found[-1]


def rom_elf(elf):
    """The ROM's ELF for the build's chip (RISC-V: the P4, Xtensa: the S3), if installed"""
    with open(elf, "rb") as f:
        machine = int.from_bytes(f.read(20)[18:20], "little")
    chip = {243: "esp32p4", 94: "esp32s3"}.get(machine)
    tools = os.environ.get("IDF_TOOLS_PATH", os.path.expanduser("~/.espressif"))
    found = sorted(glob.glob(os.path.join(tools, "tools", "esp-rom-elfs", "*", "%s_rev0_rom.elf" % chip)))
    return found[-1] if chip and found else None


def profile(port, elf, samples, top, core=1):
    syms = []
    for path in (elf, rom_elf(elf)):
        if path is None:
            continue
        out = subprocess.run([find_nm(elf), "-S", "--defined-only", path], capture_output=True, text=True).stdout
        for line in out.splitlines():
            f = line.split()
            if len(f) == 4 and f[2].lower() in "tw":
                syms.append((int(f[0], 16), int(f[1], 16), f[3]))
    syms.sort()
    starts = [s[0] for s in syms]
    port.reset_input_buffer()
    port.write(b"p %d %d\n" % (samples, core))
    pcs = []
    end = time.time() + samples / 20000 + 15
    while time.time() < end:
        line = port.readline()
        if line.startswith(b"EMUPROF END"):
            break
        if line.startswith(b"EMUPROF"):
            for x in line.split()[1:]:
                try:
                    pcs.append(int(x, 16))
                except ValueError:
                    pass
    if not pcs:
        sys.exit("no samples")
    count = collections.Counter()
    for pc in pcs:
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and pc < syms[i][0] + max(syms[i][1], 1):
            count[(syms[i][2], syms[i][0])] += 1
        else:
            count[("?%08x" % (pc & ~0xFFF), None)] += 1
    names = collections.Counter(s[2] for s in syms)
    addr2line = find_nm(elf)[:-2] + "addr2line"
    print("%d samples, core %d" % (len(pcs), core))
    for (name, start), n in count.most_common(top):
        if start is not None and names[name] > 1:     # a static name in several files: which one
            out = subprocess.run([addr2line, "-e", elf, "0x%x" % start], capture_output=True, text=True).stdout
            name += " (%s)" % os.path.basename(out.split(":")[0]) if out and not out.startswith("??") else ""
        print("%5.1f%%  %s" % (100.0 * n / len(pcs), name))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    port = open_port(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "log":
        log(port, float(sys.argv[3]) if len(sys.argv) > 3 else 10)
    elif cmd == "screenshot":
        screenshot(port, sys.argv[3] if len(sys.argv) > 3 else "screen.png")
    elif cmd == "lcd":
        screenshot(port, sys.argv[3] if len(sys.argv) > 3 else "lcd.png", b"L")
    elif cmd == "send":
        for c in sys.argv[3:]:
            port.write(c.encode() + b"\n")
            time.sleep(0.1)
    elif cmd == "info":
        port.reset_input_buffer()
        port.write(b"i\n")
        end = time.time() + 2
        while time.time() < end:
            line = port.readline()
            if line.startswith(b"EMUINFO"):
                print(line.decode().strip())
                break
    elif cmd == "profile":
        profile(port, sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 8000,
                int(sys.argv[5]) if len(sys.argv) > 5 else 25, int(sys.argv[6]) if len(sys.argv) > 6 else 1)
    elif cmd == "reset":
        if is_native_usb(sys.argv[1]):
            port.dtr = False
            port.rts = True
            time.sleep(0.1)
            port.rts = False
            port.dtr = True
        else:
            port.dtr = True     # IO0 high: boot normally
            port.rts = False    # EN low: reset
            time.sleep(0.1)
            port.rts = True
        log(port, 10)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
