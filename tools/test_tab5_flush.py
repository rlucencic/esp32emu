#!/usr/bin/env python3
"""Check actual Tab5 flush interval coverage and rotated DMA tail sizes.

The PPA and mutex calls are mocked. Requires ASan/UBSan in the C compiler.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEAD = r"""
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#define BOARD_LCD_V_RES 720
#define portMAX_DELAY 0
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
static void *s_ppa = (void *)1, *s_lock = (void *)1;
static int called, lo, hi, locks;
static int rotate(int first, int end)
{
    called++;
    lo = first;
    hi = end;
    return ESP_OK;
}
static void xSemaphoreTake(void *lock, int ticks) { locks++; }
static void xSemaphoreGive(void *lock) { locks--; }
"""
TAIL = r"""
static void check(int first, int end)
{
    int clipped_first = first < 0 ? 0 : first;
    int clipped_end = end > 720 ? 720 : end;
    called = locks = 0;
    lcd_flush(first, end);
    if (clipped_first >= clipped_end) {
        assert(!called && !locks);
        return;
    }
    assert(called == 1 && !locks);
    assert(lo >= 0 && hi <= 720 && lo < hi);
    assert(lo <= clipped_first && hi >= clipped_end);
    assert(!(lo & 7) && !(hi & 7));
    assert((clipped_first - lo) + (hi - clipped_end) <= 14);
    for (int macroblock = 16; macroblock <= 32; macroblock += 16) {
        int tail_width = (hi - lo) % 64;
        if (!tail_width) tail_width = 64;
        assert(tail_width * macroblock * 16 >= 1536);
    }
}
int main(void)
{
    unsigned cases = 0;
    for (int first = -2; first <= 722; first++) {
        for (int end = -2; end <= 722; end++) {
            check(first, end);
            cases++;
        }
    }
    int extremes[] = {INT_MIN, -1, 0, 1, 719, 720, 721, INT_MAX};
    for (int first = 0; first < 8; first++) {
        for (int end = 0; end < 8; end++) {
            check(extremes[first], extremes[end]);
            cases++;
        }
    }
    s_ppa = 0;
    called = 0;
    lcd_flush(0, 720);
    assert(!called);
    printf("PASS %u flush interval/coverage/tail cases and disabled PPA\n", cases);
}
"""


def main():
    source = (ROOT / "components/emu/lcd_tab5.c").read_text()
    begin = source.index("void lcd_flush(")
    end = source.index("\n/*", begin)
    with tempfile.TemporaryDirectory(prefix="emu-flush-test-") as temp:
        path = Path(temp) / "flush.c"
        exe = Path(temp) / "flush"
        path.write_text(HEAD + source[begin:end] + TAIL)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                       ["-O2", "-g", "-fsanitize=address,undefined", str(path), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
