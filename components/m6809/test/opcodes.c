/*
 * Every opcode once, on the 6809 and on the 6309:
 *   - its length (the PC afterwards, for instructions that don't jump)
 *     against the disassembler's;
 *   - its cycles against the MC6809E data sheet's opcode map (typed in
 *     here as the sheet lists them, indexed modes with ,X: no extra
 *     cycles); the 6309 in emulation mode must take the same;
 *   - the 6309 traps on the opcodes it doesn't have, and only on those.
 *
 *   opcodes [-v]
 */
#include <stdio.h>
#include <string.h>
#include "m6809.h"

/* The data sheet's cycles, page 0 (0: not an instruction there; the
 * undocumented 6809 opcodes aren't checked). Branches with CC = 0.
 */
static const uint8_t sheet0[256] = {
    /*        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
    /* 0 */   6, 0, 0, 6, 6, 0, 6, 6, 6, 6, 6, 0, 6, 6, 3, 6,
    /* 1 */   0, 0, 2, 4, 0, 0, 5, 9, 0, 2, 3, 0, 3, 2, 8, 6,
    /* 2 */   3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    /* 3 */   4, 4, 4, 4, 5, 5, 5, 5, 0, 5, 3, 6, 20, 11, 0, 19,
    /* 4 */   2, 0, 0, 2, 2, 0, 2, 2, 2, 2, 2, 0, 2, 2, 0, 2,
    /* 5 */   2, 0, 0, 2, 2, 0, 2, 2, 2, 2, 2, 0, 2, 2, 0, 2,
    /* 6 */   6, 0, 0, 6, 6, 0, 6, 6, 6, 6, 6, 0, 6, 6, 3, 6,
    /* 7 */   7, 0, 0, 7, 7, 0, 7, 7, 7, 7, 7, 0, 7, 7, 4, 7,
    /* 8 */   2, 2, 2, 4, 2, 2, 2, 0, 2, 2, 2, 2, 4, 7, 3, 0,
    /* 9 */   4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 7, 5, 5,
    /* A */   4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 7, 5, 5,
    /* B */   5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 5, 7, 8, 6, 6,
    /* C */   2, 2, 2, 4, 2, 2, 2, 0, 2, 2, 2, 2, 3, 0, 3, 0,
    /* D */   4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
    /* E */   4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
    /* F */   5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6,
};

/* Page 2 ($10) and 3 ($11): opcode, cycles (long branches with CC = 0) */
static const uint8_t sheet2[][2] = {
    { 0x21, 5 }, { 0x22, 6 }, { 0x23, 5 }, { 0x24, 6 }, { 0x25, 5 }, { 0x26, 6 }, { 0x27, 5 }, { 0x28, 6 },
    { 0x29, 5 }, { 0x2A, 6 }, { 0x2B, 5 }, { 0x2C, 6 }, { 0x2D, 5 }, { 0x2E, 6 }, { 0x2F, 5 },
    { 0x3F, 20 }, { 0x83, 5 }, { 0x8C, 5 }, { 0x8E, 4 }, { 0x93, 7 }, { 0x9C, 7 }, { 0x9E, 6 }, { 0x9F, 6 },
    { 0xA3, 7 }, { 0xAC, 7 }, { 0xAE, 6 }, { 0xAF, 6 }, { 0xB3, 8 }, { 0xBC, 8 }, { 0xBE, 7 }, { 0xBF, 7 },
    { 0xCE, 4 }, { 0xDE, 6 }, { 0xDF, 6 }, { 0xEE, 6 }, { 0xEF, 6 }, { 0xFE, 7 }, { 0xFF, 7 },
};
static const uint8_t sheet3[][2] = {
    { 0x3F, 20 }, { 0x83, 5 }, { 0x8C, 5 }, { 0x93, 7 }, { 0x9C, 7 }, { 0xA3, 7 }, { 0xAC, 7 }, { 0xB3, 8 }, { 0xBC, 8 },
};

static uint8_t s_mem[65536];

static uint8_t bus_read(void *ctx, uint16_t a)
{
    (void)ctx;
    return s_mem[a];
}

static void bus_write(void *ctx, uint16_t a, uint8_t v)
{
    (void)ctx;
    s_mem[a] = v;
}

typedef struct {
    int cycles;
    int length;             /* PC afterwards - start */
    bool trapped;           /* 6309: MD's IL set */
} result_t;

/* Run one instruction: prefix (0 none), opcode, operand bytes after it
 * ($84 = ,X for indexed modes, zeros for the rest)
 */
static result_t run(bool h6309, uint8_t prefix, uint8_t op)
{
    static const m6809_bus_t bus = { bus_read, bus_write, NULL };
    m6809_t c;
    memset(s_mem, 0, sizeof(s_mem));
    m6809_init(&c, &bus, h6309);
    uint16_t pc = 0x1000, at = pc;
    if (prefix) {
        s_mem[at++] = prefix;
    }
    s_mem[at++] = op;
    /* An indexed postbyte where one goes, so its mode adds nothing */
    uint8_t row = op >> 4;
    bool idx = row == 0x6 || row == 0xA || row == 0xE || (row == 0x3 && op <= 0x33 && !prefix);
    bool im_idx = h6309 && !prefix && row == 0x6 && (op == 0x61 || op == 0x62 || op == 0x65 || op == 0x6B);
    if (im_idx) {
        s_mem[at++] = 0;    /* the 6309's immediate byte, then the postbyte */
    }
    if (idx) {
        s_mem[at] = 0x84;
    }
    c.pc = pc;
    c.s = 0x8000;
    c.u = 0x7000;
    c.x = 0x2000;
    c.y = 0x3000;
    c.w = 0;
    c.nmi_armed = true;
    c.until = 1000;
    uint64_t start = c.cycles;
    m6809_step(&c);
    /* A division by zero (the operands are zeros) traps on purpose: no length */
    result_t r = { (int)(c.cycles - start), (c.md & M6309_DZ) ? -1 : (int)(uint16_t)(c.pc - pc),
                   (c.md & M6309_IL) != 0 };
    return r;
}

static bool is_flow(uint8_t prefix, uint8_t op)
{
    if (prefix == 0) {
        return op == 0x0E || op == 0x6E || op == 0x7E || op == 0x16 || op == 0x17 || op == 0x39 ||
               op == 0x3B || op == 0x3F || op == 0x8D || op == 0x9D || op == 0xAD || op == 0xBD ||
               op == 0x3C || op == 0x13 || op == 0x3E || op == 0x35 || op == 0x37;
    }
    return op == 0x3F;
}

static int dis_length(bool h6309, uint8_t prefix, uint8_t op)
{
    static const m6809_bus_t bus = { bus_read, bus_write, NULL };
    m6809_t c;
    m6809_init(&c, &bus, h6309);
    /* the memory is still the last run's: write the instruction again */
    memset(s_mem + 0x1000, 0, 8);
    uint16_t at = 0x1000;
    if (prefix) {
        s_mem[at++] = prefix;
    }
    s_mem[at++] = op;
    uint8_t row = op >> 4;
    bool idx = row == 0x6 || row == 0xA || row == 0xE || (row == 0x3 && op <= 0x33 && !prefix);
    bool im_idx = h6309 && !prefix && row == 0x6 && (op == 0x61 || op == 0x62 || op == 0x65 || op == 0x6B);
    if (im_idx) {
        s_mem[at++] = 0;
    }
    if (idx) {
        s_mem[at] = 0x84;
    }
    /* the pages: flat memory for m6809_peek() */
    for (int p = 0; p < 256; p++) {
        c.rd[p] = s_mem + p * 256;
    }
    char buf[64];
    return m6809_disassemble(&c, 0x1000, buf, sizeof(buf));
}

int main(int argc, char **argv)
{
    bool verbose = argc > 1 && !strcmp(argv[1], "-v");
    int failures = 0, checked = 0;

    /* ---- 6809 and 6309 (emulation mode) against the sheet ---- */
    for (int cpu = 0; cpu < 2; cpu++) {
        for (int page = 0; page < 3; page++) {
            for (int op = 0; op < 256; op++) {
                int want = 0;
                uint8_t prefix = page == 0 ? 0 : page == 1 ? 0x10 : 0x11;
                if (page == 0) {
                    if (op == 0x10 || op == 0x11) {
                        continue;
                    }
                    want = sheet0[op];
                } else {
                    const uint8_t (*t)[2] = page == 1 ? sheet2 : sheet3;
                    int n = page == 1 ? (int)(sizeof(sheet2) / 2) : (int)(sizeof(sheet3) / 2);
                    for (int i = 0; i < n; i++) {
                        if (t[i][0] == op) {
                            want = t[i][1];
                        }
                    }
                }
                if (!want) {
                    continue;
                }
                result_t r = run(cpu, prefix, (uint8_t)op);
                checked++;
                bool ok = r.cycles == want && !r.trapped;
                int dl = dis_length(cpu, prefix, (uint8_t)op);
                bool len_ok = is_flow(prefix, (uint8_t)op) || r.length == dl;
                if (!ok || !len_ok || verbose) {
                    printf("%s %s %s$%02X: %d cycles (sheet %d), length %d (disassembler %d)%s\n",
                           ok && len_ok ? "ok  " : "FAIL", cpu ? "6309" : "6809", prefix == 0x10 ? "$10 " : prefix ? "$11 " : "",
                           op, r.cycles, want, r.length, dl, r.trapped ? " TRAPPED" : "");
                }
                if (!ok || !len_ok) {
                    failures++;
                }
            }
        }
    }

    /* ---- The 6309: what traps and what doesn't, lengths ---- */
    int traps = 0, legal = 0;
    for (int page = 0; page < 3; page++) {
        for (int op = 0; op < 256; op++) {
            uint8_t prefix = page == 0 ? 0 : page == 1 ? 0x10 : 0x11;
            if (page == 0 && (op == 0x10 || op == 0x11)) {
                continue;
            }
            /* the disassembler knows the instruction set: FCB = not one */
            static const m6809_bus_t bus = { bus_read, bus_write, NULL };
            m6809_t c;
            m6809_init(&c, &bus, true);
            memset(s_mem, 0, sizeof(s_mem));
            uint16_t at = 0x1000;
            if (prefix) {
                s_mem[at++] = prefix;
            }
            s_mem[at] = (uint8_t)op;
            for (int p = 0; p < 256; p++) {
                c.rd[p] = s_mem + p * 256;
            }
            char buf[64];
            m6809_disassemble(&c, 0x1000, buf, sizeof(buf));
            bool known = strncmp(buf, "FCB", 3) != 0;
            result_t r = run(true, prefix, (uint8_t)op);
            checked++;
            if (r.trapped) {
                traps++;
            } else {
                legal++;
            }
            bool ok = known != r.trapped;
            int dl = dis_length(true, prefix, (uint8_t)op);
            bool len_ok = !known || is_flow(prefix, (uint8_t)op) || r.trapped || r.length == dl || r.length < 0 ||
                          (prefix == 0x11 && op >= 0x38 && op <= 0x3B);
            if (!ok || !len_ok || verbose) {
                printf("%s 6309 %s$%02X %-14s %s, length %d (disassembler %d)\n", ok && len_ok ? "ok  " : "FAIL",
                       prefix == 0x10 ? "$10 " : prefix ? "$11 " : "", op, buf, r.trapped ? "traps" : "runs",
                       r.length, dl);
            }
            if (!ok || !len_ok) {
                failures++;
            }
        }
    }
    printf("%d checks (the 6309: %d opcodes run, %d trap), %d failed\n", checked, legal, traps, failures);
    return failures ? 1 : 0;
}
