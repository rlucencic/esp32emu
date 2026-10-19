/*
 * ARM2 disassembler: the syntax of RISC OS's own (ADDEQS R0,R1,#&10;
 * LDMIA R13!,{R0-R3,PC}^; & for hex).
 */
#include <stdarg.h>
#include <stdio.h>
#include "arm2.h"

static const char *const s_cond[16] = {
    "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
    "HI", "LS", "GE", "LT", "GT", "LE", "", "NV",
};

static const char *const s_dp[16] = {
    "AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC",
    "TST", "TEQ", "CMP", "CMN", "ORR", "MOV", "BIC", "MVN",
};

static const char *const s_shift[4] = { "LSL", "LSR", "ASR", "ROR" };

static const char *const s_reg[16] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "PC",
};

typedef struct {
    char *buf;
    int len, at;
} out_t;

static void put(out_t *o, const char *fmt, ...)
{
    if (o->at >= o->len) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->at, o->len - o->at, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o->at += n;
        if (o->at > o->len) {
            o->at = o->len;
        }
    }
}

static void hex(out_t *o, uint32_t v)
{
    put(o, v < 10 ? "%u" : "&%X", (unsigned)v);
}

/* Rm with its shift (data processing, and LDR/STR's register offsets) */
static void shifted_register(out_t *o, uint32_t op, bool by_register)
{
    unsigned type = (op >> 5) & 3;
    put(o, "%s", s_reg[op & 15]);
    if (by_register && (op & 0x10)) {
        put(o, ",%s %s", s_shift[type], s_reg[(op >> 8) & 15]);
        return;
    }
    unsigned amount = (op >> 7) & 31;
    if (amount == 0) {
        if (type == 0) {
            return;
        }
        if (type == 3) {
            put(o, ",RRX");
            return;
        }
        amount = 32;
    }
    put(o, ",%s #%u", s_shift[type], amount);
}

static void register_list(out_t *o, uint32_t list)
{
    put(o, "{");
    bool first = true;
    for (int i = 0; i < 16; i++) {
        if (!(list & (1u << i))) {
            continue;
        }
        int j = i;
        while (j < 15 && (list & (1u << (j + 1)))) {
            j++;
        }
        put(o, "%s%s", first ? "" : ",", s_reg[i]);
        if (j > i + 1) {
            put(o, "-%s", s_reg[j]);
            i = j;
        } else if (j == i + 1) {
            put(o, ",%s", s_reg[j]);
            i = j;
        }
        first = false;
    }
    put(o, "}");
}

void arm2_disassemble_word(uint32_t op, uint32_t addr, char *buf, int len)
{
    out_t o = { buf, len, 0 };
    const char *cond = s_cond[op >> 28];
    buf[0] = 0;

    switch ((op >> 25) & 7) {
    case 0:
    case 1:
        if (!(op & (1u << 25)) && (op & 0x90) == 0x90) {
            if ((op & 0x0FC000F0) == 0x00000090) {
                bool acc = op & (1u << 21);
                put(&o, "%s%s%s %s,%s,%s", acc ? "MLA" : "MUL", cond, (op & (1u << 20)) ? "S" : "",
                    s_reg[(op >> 16) & 15], s_reg[op & 15], s_reg[(op >> 8) & 15]);
                if (acc) {
                    put(&o, ",%s", s_reg[(op >> 12) & 15]);
                }
            } else if ((op & 0x0FB00FF0) == 0x01000090) {
                put(&o, "SWP%s%s %s,%s,[%s]", cond, (op & (1u << 22)) ? "B" : "", s_reg[(op >> 12) & 15],
                    s_reg[op & 15], s_reg[(op >> 16) & 15]);
            } else {
                put(&o, "Undefined");
            }
            return;
        }
        {
            unsigned opc = (op >> 21) & 15;
            unsigned rd = (op >> 12) & 15, rn = (op >> 16) & 15;
            bool s = op & (1u << 20);
            bool test = opc >= 8 && opc <= 11;
            put(&o, "%s%s%s", s_dp[opc], cond, test ? (rd == 15 ? "P" : "") : (s ? "S" : ""));
            if (test && !s) {
                o.at = 0;
                put(&o, "Undefined");
                return;
            }
            put(&o, " ");
            if (!test) {
                put(&o, "%s,", s_reg[rd]);
            }
            if (opc != 13 && opc != 15) {
                put(&o, "%s,", s_reg[rn]);
            }
            if (op & (1u << 25)) {
                unsigned rot = (op >> 7) & 0x1E;
                uint32_t v = op & 0xFF;
                v = rot ? (v >> rot) | (v << (32 - rot)) : v;
                put(&o, "#");
                hex(&o, v);
            } else {
                shifted_register(&o, op, true);
            }
        }
        return;

    case 2:
    case 3: {
        if ((op & (1u << 25)) && (op & 0x10)) {
            put(&o, "Undefined");
            return;
        }
        bool pre = op & (1u << 24), up = op & (1u << 23), byte = op & (1u << 22);
        bool wb = op & (1u << 21), load = op & (1u << 20);
        unsigned rn = (op >> 16) & 15;
        put(&o, "%s%s%s%s %s,", load ? "LDR" : "STR", cond, byte ? "B" : "", (!pre && wb) ? "T" : "",
            s_reg[(op >> 12) & 15]);
        if (!(op & (1u << 25)) && rn == 15 && pre && !wb) {
            /* PC relative: the address */
            uint32_t off = op & 0xFFF;
            put(&o, "&%08X", (unsigned)((addr + 8 + (up ? off : -off)) & ARM2_ADDR_MASK));
            return;
        }
        put(&o, "[%s", s_reg[rn]);
        if (!pre) {
            put(&o, "]");
        }
        if (op & (1u << 25)) {
            put(&o, ",%s", up ? "" : "-");
            shifted_register(&o, op, false);
        } else if (op & 0xFFF) {
            put(&o, ",#%s", up ? "" : "-");
            hex(&o, op & 0xFFF);
        }
        if (pre) {
            put(&o, "]%s", wb ? "!" : "");
        }
        return;
    }

    case 4: {
        static const char *const modes[4] = { "DA", "IA", "DB", "IB" };
        unsigned rn = (op >> 16) & 15;
        put(&o, "%s%s%s %s%s,", (op & (1u << 20)) ? "LDM" : "STM", cond, modes[(op >> 23) & 3], s_reg[rn],
            (op & (1u << 21)) ? "!" : "");
        register_list(&o, op & 0xFFFF);
        if (op & (1u << 22)) {
            put(&o, "^");
        }
        return;
    }

    case 5: {
        int32_t off = (int32_t)(op << 8) >> 6;
        put(&o, "%s%s &%08X", (op & (1u << 24)) ? "BL" : "B", cond,
            (unsigned)((addr + 8 + (uint32_t)off) & ARM2_ADDR_MASK));
        return;
    }

    case 6: {
        bool pre = op & (1u << 24), up = op & (1u << 23), wb = op & (1u << 21);
        put(&o, "%s%s%s P%u,C%u,[%s", (op & (1u << 20)) ? "LDC" : "STC", cond, (op & (1u << 22)) ? "L" : "",
            (unsigned)((op >> 8) & 15), (unsigned)((op >> 12) & 15), s_reg[(op >> 16) & 15]);
        if (!pre) {
            put(&o, "]");
        }
        if (op & 0xFF) {
            put(&o, ",#%s", up ? "" : "-");
            hex(&o, (op & 0xFF) * 4);
        }
        if (pre) {
            put(&o, "]%s", wb ? "!" : "");
        }
        return;
    }

    default:
        if (op & (1u << 24)) {
            put(&o, "SWI%s &%X", cond, (unsigned)(op & 0xFFFFFF));
        } else if (op & 0x10) {
            put(&o, "%s%s P%u,%u,%s,C%u,C%u,%u", (op & (1u << 20)) ? "MRC" : "MCR", cond,
                (unsigned)((op >> 8) & 15), (unsigned)((op >> 21) & 7), s_reg[(op >> 12) & 15],
                (unsigned)((op >> 16) & 15), (unsigned)(op & 15), (unsigned)((op >> 5) & 7));
        } else {
            put(&o, "CDP%s P%u,%u,C%u,C%u,C%u,%u", cond, (unsigned)((op >> 8) & 15), (unsigned)((op >> 20) & 15),
                (unsigned)((op >> 12) & 15), (unsigned)((op >> 16) & 15), (unsigned)(op & 15),
                (unsigned)((op >> 5) & 7));
        }
        return;
    }
}

void arm2_disassemble(const arm2_t *cpu, uint32_t addr, char *buf, int len)
{
    arm2_disassemble_word(arm2_peek32(cpu, addr), addr & ARM2_PC, buf, len);
}
