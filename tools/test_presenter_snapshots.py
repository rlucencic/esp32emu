#!/usr/bin/env python3
"""Exercise actual presenter snapshots with adversarial source changes.

The IDF/task/heap/LCD boundaries are mocked; presenter helpers and loop
come directly from emu.c. Requires a C compiler with ASan/UBSan.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEAD = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#define BOARD_LCD_H_RES 1280
#define BOARD_LCD_V_RES 12
#define VIEW_H 12
#define PRESENT_MS 40
#define EMU_SCALE_SMOOTH 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define pdMS_TO_TICKS(x) (x)

typedef struct {
    int width, height, hz, src_x, src_w, view_w, view_h;
} emu_screen_t;
static emu_screen_t cfg;
static int frame, reads, dynamic, allocated;
static bool s_present_on = true;
static jmp_buf done;
static void *allocs[16];

static void *heap_caps_malloc(size_t n, int caps)
{
    void *p = malloc(n);
    assert(p && allocated < 16);
    allocs[allocated++] = p;
    return p;
}
static void *heap_caps_calloc(size_t n, size_t size, int caps)
{
    void *p = heap_caps_malloc(n * size, caps);
    memset(p, 0, n * size);
    return p;
}
static int emu_view_width(void) { return cfg.view_w; }
static void delay(int unused)
{
    if (++frame > 3) longjmp(done, 1);
    reads = 0;
}
#define vTaskDelay delay
static uint32_t signature(void) { return frame; }
static void screenmode(emu_screen_t *mode) { *mode = cfg; }
static void render(int y, uint16_t *out)
{
    /* Detection sees A, scaling sees B, then the source settles on A. */
    bool b = dynamic && frame == 1 && reads >= cfg.height;
    reads++;
    for (int x = 0; x < cfg.width; x++) {
        out[x] = (b ? 0xa002 : 0x1123) + y * 113 + x * 19;
    }
}
static void lcd_flush(int first, int end) {}
static struct {
    int scale;
    uint32_t (*screen_signature)(void);
    void (*screen_mode)(emu_screen_t *);
    void (*render_line)(int, uint16_t *);
} machine = {0, signature, screenmode, render}, *M = &machine;
"""
TAIL = r"""
static void run(int adversarial, uint16_t *fb)
{
    frame = allocated = 0;
    dynamic = adversarial;
    memset(fb, 0, BOARD_LCD_H_RES * VIEW_H * sizeof(*fb));
    if (!setjmp(done)) present_task(fb);
    for (int i = 0; i < allocated; i++) free(allocs[i]);
}
int main(void)
{
    size_t bytes = BOARD_LCD_H_RES * VIEW_H * sizeof(uint16_t);
    uint16_t *reference = malloc(bytes), *adversarial = malloc(bytes);
    assert(reference && adversarial);
    unsigned cases = 0;
    for (int smooth = 0; smooth < 2; smooth++) {
        for (int crop = 0; crop < 2; crop++) {
            for (int width = 4; width <= 8; width += 2) {
                for (int height = 4; height <= 12; height += 4) {
                    cfg = (emu_screen_t){
                        .width = crop ? 6 : 4, .height = 4,
                        .src_x = crop, .src_w = 4,
                        .view_w = width, .view_h = height,
                    };
                    machine.scale = smooth;
                    run(0, reference);
                    run(1, adversarial);
                    assert(!memcmp(reference, adversarial, bytes));
                    cases++;
                }
            }
        }
    }
    printf("PASS %u actual-presenter adversarial snapshot cases\n", cases);
    free(reference);
    free(adversarial);
}
"""


def main():
    source = (ROOT / "components/emu/emu.c").read_text()
    begin = source.index("static inline uint32_t expand565(")
    end = source.index("/* ---- Setup screen", begin)
    code = HEAD + source[begin:end] + TAIL
    with tempfile.TemporaryDirectory(prefix="emu-presenter-test-") as temp:
        path = Path(temp) / "presenter.c"
        exe = Path(temp) / "presenter"
        path.write_text(code)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                       ["-O2", "-g", "-fsanitize=address,undefined", str(path), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
