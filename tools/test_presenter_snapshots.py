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
#define BOARD_LCD_V_RES 16
#define VIEW_H 16
#define PRESENT_MS 40
#define EMU_SCALE_SMOOTH 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_DMA 4
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define LCD_STRIP_ROWS 8
#define LCD_STRIP_COLS 32
static int strips_on;               /* the Tab5's way, or the MaTouch's */
#define LCD_STRIPS strips_on
#if BUDGET_TEST
#define PRESENT_ROWS_PER_S 4        /* the first round's rows: 1-4 s */
#endif

typedef struct {
    int width, height, hz, src_x, src_w, view_w, view_h;
} emu_screen_t;
static emu_screen_t cfg;
static int frame, reads, dynamic, allocated, frames_max;
static bool s_present_on = true;
/* The keyed cases: each line's picture from its version (and hidden, a
 * change its key doesn't see), the signature bumped with every change */
static int schedule, bumped, version[16], hidden[16];
static uint32_t sig;
static void apply_schedule(void)
{
    /* (round 1 draws every line) */
    if (schedule == 1 && frame == 2) {
        version[1]++, sig++;
    } else if (schedule == 1 && frame == 4) {
        version[2]++, version[3]++, sig++;
    } else if (schedule == 2 && frame == 2) {
        version[0]++, sig++;        /* line 0 drawn in this round ... */
    } else if (schedule == 3 && frame == 2) {
        hidden[2]++, sig++;         /* a change its key misses: the audit's */
    } else if (schedule == 4 && (frame == 2 || frame == 3)) {
        version[frame - 1]++, sig++;    /* (while the budget waits) */
    } else if (schedule == 5 && frame == 3) {
        cfg.view_w = cfg.view_w == 8 ? 4 : 8, sig++;    /* the picture narrower or wider */
    }
}
static jmp_buf done;
static uint16_t copy[BOARD_LCD_H_RES * VIEW_H];  /* what present_task() gets */
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
static void *heap_caps_aligned_calloc(size_t align, size_t n, size_t size, int caps)
{
    return heap_caps_calloc(n, size, caps);
}
static int emu_view_width(void) { return cfg.view_w; }
static void delay(int unused)
{
    if (++frame > frames_max) longjmp(done, 1);
    reads = 0;
    apply_schedule();
}
#define vTaskDelay delay
static void notified(int clear, int ticks) { delay(ticks); }
#define ulTaskNotifyTake notified
static int64_t esp_timer_get_time(void) { return (int64_t)frame * PRESENT_MS * 1000; }
static uint32_t signature(void) { return schedule ? sig : frame; }
static void screenmode(emu_screen_t *mode) { *mode = cfg; }
static uint32_t key(int y) { return version[y]; }
static void render(int y, uint16_t *out)
{
    if (schedule) {
        for (int x = 0; x < cfg.width; x++) {
            out[x] = 0x1123 + version[y] * 977 + hidden[y] * 31 + y * 113 + x * 19;
        }
        /* ... and changed again right after it was rendered: a key read
         * after the line would describe a picture never drawn */
        if (schedule == 2 && frame == 2 && y == 0 && !bumped) {
            bumped = 1;
            version[0]++, sig++;
        }
        return;
    }
    /* Detection sees A, scaling sees B, then the source settles on A. */
    bool b = dynamic && frame == 1 && reads >= cfg.height;
    reads++;
    for (int x = 0; x < cfg.width; x++) {
        out[x] = (b ? 0xa002 : 0x1123) + y * 113 + x * 19;
    }
}
/* The panel: the copy's rows turned onto it (lcd_flush()), or strips */
static uint16_t panel[BOARD_LCD_H_RES * VIEW_H];
static int strips_put;
static void lcd_flush(int first, int end)
{
    for (int y = first; y < end; y++) {
        memcpy(panel + y * BOARD_LCD_H_RES, copy + y * BOARD_LCD_H_RES, BOARD_LCD_H_RES * sizeof(uint16_t));
    }
}
static void lcd_put_strip(const uint16_t *pixels, int x, int w, int y, int n)
{
    assert(x % LCD_STRIP_COLS == 0 && w % LCD_STRIP_COLS == 0 && x + w <= BOARD_LCD_H_RES);
    assert(y % LCD_STRIP_ROWS == 0 && n % LCD_STRIP_ROWS == 0 && y + n <= VIEW_H);
    for (int k = 0; k < n; k++) {
        memcpy(panel + (y + k) * BOARD_LCD_H_RES + x, pixels + k * w, w * sizeof(uint16_t));
    }
    strips_put++;
}
static struct {
    int scale;
    uint32_t (*screen_signature)(void);
    void (*screen_mode)(emu_screen_t *);
    void (*render_line)(int, uint16_t *);
    uint32_t (*line_key)(int);
} machine = {0, signature, screenmode, render, NULL}, *M = &machine;
"""
TAIL = r"""
#define AUDIT_ROUNDS (AUDIT_MS / PRESENT_MS)
static void run(int adversarial, uint16_t *result, int strips)
{
    frame = allocated = bumped = 0;
    sig = 1;
    memset(version, 0, sizeof(version));
    memset(hidden, 0, sizeof(hidden));
    dynamic = adversarial;
    strips_on = strips;
    emu_screen_t was = cfg;
    memset(copy, 0, sizeof(copy));
    memset(panel, 0x55, sizeof(panel));     /* (the presenter's own clear blackens it) */
    if (!setjmp(done)) present_task(copy);
    for (int i = 0; i < allocated; i++) free(allocs[i]);
    memcpy(result, panel, sizeof(panel));
    cfg = was;
}
/* A schedule with the line keys (into the framebuffer or by strips)
 * against the same without them, into the framebuffer
 */
static void keyed(int s, int frames, bool expect_same, uint16_t *reference, uint16_t *candidate,
                  size_t bytes, int strips)
{
    schedule = s;
    frames_max = frames;
    machine.line_key = NULL;
    run(0, reference, 0);
    machine.line_key = key;
    run(0, candidate, strips);
    machine.line_key = NULL;
    schedule = 0;
    assert(!memcmp(reference, candidate, bytes) == expect_same);
}
int main(void)
{
    size_t bytes = sizeof(panel);
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
#if BUDGET_TEST
                    /* A change while the budget waits shows once it's spent:
                     * not before (the first round's 4-16 rows: 25-100 frames) */
                    for (int strips = 0; strips < 2; strips++) {
                        keyed(4, 20, false, reference, adversarial, bytes, strips);
                        keyed(4, 110, true, reference, adversarial, bytes, strips);
                        cases += 2;
                    }
#else
                    /* Every picture against the framebuffer's, unkeyed */
                    for (int strips = 0; strips < 2; strips++) {
                        frames_max = 3;
                        run(0, reference, 0);
                        run(1, adversarial, strips);
                        assert(!memcmp(reference, adversarial, bytes));
                        cases++;
                        /* Line keys: changes in later rounds, a change right
                         * after a line was rendered, a change no key saw (put
                         * right by the audit round, not before), the
                         * picture's size changed */
                        keyed(1, 5, true, reference, adversarial, bytes, strips);
                        keyed(2, 5, true, reference, adversarial, bytes, strips);
                        keyed(3, AUDIT_ROUNDS - 1, false, reference, adversarial, bytes, strips);
                        keyed(3, AUDIT_ROUNDS + 2, true, reference, adversarial, bytes, strips);
                        keyed(5, 6, true, reference, adversarial, bytes, strips);
                        cases += 5;
                    }
#endif
                }
            }
        }
    }
    assert(strips_put > 0);
    printf("PASS %u actual-presenter adversarial snapshot cases (budget %d)\n", cases, BUDGET_TEST);
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
        # The usual budget and one that makes rounds wait; each case into
        # the framebuffer (the MaTouch's way) and by strips (the Tab5's)
        for budget in (0, 1):
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                           ["-O2", "-g", "-fsanitize=address,undefined",
                            "-DBUDGET_TEST=%d" % budget, str(path), "-o", str(exe)],
                           check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
