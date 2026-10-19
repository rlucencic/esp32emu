/*
 * The ARM2 core for diff.py (ctypes): 64 KB of RAM at 0, user mode.
 */
#include <stdlib.h>
#include <string.h>
#include "arm2.h"

#define MEM_SIZE    (64 * 1024)

static uint8_t *s_mem;
static uintptr_t s_page[ARM2_PAGES];
static arm2_t s_cpu;

static uint32_t bus_read(void *ctx, uint32_t addr, bool byte, bool *abort)
{
    *abort = true;
    return 0;
}

static void bus_write(void *ctx, uint32_t addr, uint32_t v, bool byte, bool *abort)
{
    *abort = true;
}

uint8_t *diff_init(void)
{
    if (!s_mem) {
        s_mem = aligned_alloc(4096, MEM_SIZE);
    }
    memset(s_mem, 0, MEM_SIZE);
    for (uint32_t p = 0; p < MEM_SIZE / ARM2_PAGE_SIZE; p++) {
        s_page[p] = (uintptr_t)(s_mem + p * ARM2_PAGE_SIZE) | ARM2_USER_R | ARM2_USER_W | ARM2_PRIV_R | ARM2_PRIV_W;
    }
    arm2_bus_t bus = { .read = bus_read, .write = bus_write };
    arm2_init(&s_cpu, &bus, s_page);
    arm2_reset(&s_cpu);
    arm2_set_psr(&s_cpu, ARM2_USR);
    return s_mem;
}

/* regs: R0-R14, then the PC; nzcv in bits 31-28 */
void diff_set(const uint32_t *regs, uint32_t nzcv)
{
    memcpy(s_cpu.r, regs, 15 * sizeof(uint32_t));
    s_cpu.pc = regs[15];
    s_cpu.psr = (s_cpu.psr & 0x0FFFFFFF) | (nzcv & 0xF0000000);
}

/* One instruction; the vector it went to (an exception), else -1 */
int diff_step(void)
{
    uint32_t before = s_cpu.pc;
    s_cpu.left = 0;
    arm2_run(&s_cpu, 1);
    if ((s_cpu.psr & ARM2_MODE) != ARM2_USR) {
        return (int)s_cpu.pc;
    }
    (void)before;
    return -1;
}

void diff_get(uint32_t *regs, uint32_t *nzcv)
{
    memcpy(regs, s_cpu.r, 15 * sizeof(uint32_t));
    regs[15] = s_cpu.pc;
    *nzcv = s_cpu.psr & 0xF0000000;
}
