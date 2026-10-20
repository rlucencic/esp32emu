/*
 * x86 disassembler (8086 and the 286's real-mode instructions), DOS DEBUG
 * style: "MOV AX,[BX+SI+12]", "ADD BYTE PTR [0100],05", "JZ 0123"
 */
#include <stdio.h>
#include <string.h>
#include "x86.h"

typedef struct {
    const uint8_t *bytes;
    int pos, max;
    uint16_t off;                   /* the instruction's offset */
    bool i286;
    const char *seg;                /* override, "ES" ... or NULL */
    char *out;
    int len, n;
} dis_t;

static const char *const reg8[] = { "AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH" };
static const char *const reg16[] = { "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI" };
static const char *const sreg[] = { "ES", "CS", "SS", "DS" };
static const char *const alu[] = { "ADD", "OR", "ADC", "SBB", "AND", "SUB", "XOR", "CMP" };
static const char *const shifts[] = { "ROL", "ROR", "RCL", "RCR", "SHL", "SHR", "SETMO", "SAR" };
static const char *const grp3[] = { "TEST", "TEST", "NOT", "NEG", "MUL", "IMUL", "DIV", "IDIV" };
static const char *const jcc[] = {
    "JO", "JNO", "JB", "JNB", "JZ", "JNZ", "JBE", "JA", "JS", "JNS", "JPE", "JPO", "JL", "JGE", "JLE", "JG"
};
static const char *const bases[] = { "BX+SI", "BX+DI", "BP+SI", "BP+DI", "SI", "DI", "BP", "BX" };

static uint8_t b8(dis_t *d)
{
    return d->pos < d->max ? d->bytes[d->pos++] : 0;
}

static uint16_t b16(dis_t *d)
{
    uint16_t lo = b8(d);
    return lo | b8(d) << 8;
}

static void put(dis_t *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void put(dis_t *d, const char *fmt, ...)
{
    if (d->n >= d->len) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(d->out + d->n, (size_t)(d->len - d->n), fmt, ap);
    va_end(ap);
    if (k > 0) {
        d->n += k;
        if (d->n > d->len) {
            d->n = d->len;
        }
    }
}

typedef struct {
    int mod, reg, rm;
    uint16_t disp;
} mrm_t;

static mrm_t modrm(dis_t *d)
{
    mrm_t m;
    uint8_t b = b8(d);
    m.mod = b >> 6;
    m.reg = (b >> 3) & 7;
    m.rm = b & 7;
    m.disp = 0;
    if (m.mod == 1) {
        m.disp = (uint16_t)(int8_t)b8(d);
    } else if (m.mod == 2 || (m.mod == 0 && m.rm == 6)) {
        m.disp = b16(d);
    }
    return m;
}

/* The r/m operand; size: 0 none, 1 BYTE PTR, 2 WORD PTR, 3 DWORD PTR (when ambiguous) */
static void rm_op(dis_t *d, const mrm_t *m, int w, int size)
{
    if (m->mod == 3) {
        put(d, "%s", w ? reg16[m->rm] : reg8[m->rm]);
        return;
    }
    static const char *const sizes[] = { "", "BYTE PTR ", "WORD PTR ", "DWORD PTR ", "QWORD PTR ", "TBYTE PTR " };
    put(d, "%s", sizes[size]);
    if (d->seg) {
        put(d, "%s:", d->seg);
    }
    if (m->mod == 0 && m->rm == 6) {
        put(d, "[%04X]", m->disp);
    } else if (m->mod == 0) {
        put(d, "[%s]", bases[m->rm]);
    } else if (m->mod == 1) {
        int8_t v = (int8_t)m->disp;
        put(d, "[%s%c%02X]", bases[m->rm], v < 0 ? '-' : '+', v < 0 ? -v : v);
    } else {
        put(d, "[%s+%04X]", bases[m->rm], m->disp);
    }
}

static void mnem(dis_t *d, const char *name)
{
    put(d, "%-7s ", name);
}

static void rel(dis_t *d, int len_after, int16_t delta)
{
    put(d, "%04X", (uint16_t)(d->off + len_after + delta));
}

/* The coprocessor's instructions (ESC: D8-DF), as Intel names them (the
 * no-wait forms FN...: a WAIT before them is an instruction of its own)
 */
static void esc(dis_t *d, uint8_t op)
{
    static const char *const arith[] = { "FADD", "FMUL", "FCOM", "FCOMP", "FSUB", "FSUBR", "FDIV", "FDIVR" };
    static const char *const iarith[] = { "FIADD", "FIMUL", "FICOM", "FICOMP", "FISUB", "FISUBR", "FIDIV", "FIDIVR" };
    static const char *const mem[8][8] = {
        { 0 },
        { "FLD", NULL, "FST", "FSTP", "FLDENV", "FLDCW", "FNSTENV", "FNSTCW" },
        { 0 },
        { "FILD", NULL, "FIST", "FISTP", NULL, "FLD", NULL, "FSTP" },
        { 0 },
        { "FLD", NULL, "FST", "FSTP", "FRSTOR", NULL, "FNSAVE", "FNSTSW" },
        { 0 },
        { "FILD", NULL, "FIST", "FISTP", "FBLD", "FILD", "FBSTP", "FISTP" },
    };
    /* 2 WORD, 3 DWORD, 4 QWORD, 5 TBYTE, 0 none */
    static const uint8_t mem_size[8][8] = {
        { 3, 3, 3, 3, 3, 3, 3, 3 }, { 3, 0, 3, 3, 0, 2, 0, 2 }, { 3, 3, 3, 3, 3, 3, 3, 3 },
        { 3, 0, 3, 3, 0, 5, 0, 5 }, { 4, 4, 4, 4, 4, 4, 4, 4 }, { 4, 0, 4, 4, 0, 0, 0, 2 },
        { 2, 2, 2, 2, 2, 2, 2, 2 }, { 2, 0, 2, 2, 5, 4, 5, 4 },
    };
    mrm_t m = modrm(d);
    int g = op & 7;
    if (m.mod != 3) {
        const char *name = (g & 1) ? mem[g][m.reg] : (g == 0 || g == 4) ? arith[m.reg] : iarith[m.reg];
        if (name == NULL) {
            mnem(d, "ESC");
            put(d, "%02X,", g << 3 | m.reg);
            rm_op(d, &m, 1, 0);
            return;
        }
        mnem(d, name);
        rm_op(d, &m, 1, mem_size[g][m.reg]);
        return;
    }
    static const char *const d9[4][8] = {
        { "FCHS", "FABS", NULL, NULL, "FTST", "FXAM", NULL, NULL },
        { "FLD1", "FLDL2T", "FLDL2E", "FLDPI", "FLDLG2", "FLDLN2", "FLDZ", NULL },
        { "F2XM1", "FYL2X", "FPTAN", "FPATAN", "FXTRACT", "FPREM1", "FDECSTP", "FINCSTP" },
        { "FPREM", "FYL2XP1", "FSQRT", "FSINCOS", "FRNDINT", "FSCALE", "FSIN", "FCOS" },
    };
    static const char *const db[] = { "FNENI", "FNDISI", "FNCLEX", "FNINIT", "FSETPM", NULL, NULL, NULL };
    /* DC and DE: the destination ST(i), SUB/SUBR and DIV/DIVR named the other way round */
    static const char *const dc[] = { "FADD", "FMUL", "FCOM", "FCOMP", "FSUBR", "FSUB", "FDIVR", "FDIV" };
    static const char *const de[] = { "FADDP", "FMULP", "FCOMP", NULL, "FSUBRP", "FSUBP", "FDIVRP", "FDIVP" };
    static const char *const dd[] = { "FFREE", "FXCH", "FST", "FSTP", "FUCOM", "FUCOMP", NULL, NULL };
    const char *name = NULL;
    int form = 0;                   /* 0 none, 1 ST(i), 2 ST,ST(i), 3 ST(i),ST */
    switch (g) {
    case 0: name = arith[m.reg]; form = m.reg == 2 || m.reg == 3 ? 1 : 2; break;
    case 1:
        if (m.reg == 0) { name = "FLD"; form = 1; }
        else if (m.reg == 1) { name = "FXCH"; form = 1; }
        else if (m.reg == 2 && m.rm == 0) { name = "FNOP"; }
        else if (m.reg >= 4) { name = d9[m.reg - 4][m.rm]; }
        break;
    case 3: name = m.reg == 4 ? db[m.rm] : NULL; break;
    case 4: name = dc[m.reg]; form = m.reg == 2 || m.reg == 3 ? 1 : 3; break;
    case 5: name = dd[m.reg]; form = 1; break;
    case 6:
        if (m.reg == 3) { name = m.rm == 1 ? "FCOMPP" : NULL; }
        else { name = de[m.reg]; form = m.reg == 2 ? 1 : 3; }
        break;
    case 7: name = m.reg == 4 && m.rm == 0 ? "FNSTSW" : NULL; break;
    default: break;
    }
    if (name == NULL) {
        mnem(d, "ESC");
        put(d, "%02X,ST(%d)", g << 3 | m.reg, m.rm);
        return;
    }
    if (g == 7) {
        mnem(d, name);
        put(d, "AX");
        return;
    }
    if (form == 0) {
        put(d, "%s", name);
        return;
    }
    mnem(d, name);
    put(d, form == 1 ? "ST(%d)" : form == 2 ? "ST,ST(%d)" : "ST(%d),ST", m.rm);
}

static int decode(dis_t *d)
{
    const char *rep = NULL;
    uint8_t op;
    for (;;) {
        op = b8(d);
        if ((op & 0xE7) == 0x26) {
            d->seg = sreg[(op >> 3) & 3];
        } else if (op == 0xF0 || (op == 0xF1 && !d->i286)) {
            put(d, "LOCK ");
        } else if (op == 0xF2) {
            rep = "REPNZ ";
        } else if (op == 0xF3) {
            rep = "REPZ ";
        } else {
            break;
        }
        if (d->pos >= d->max) {
            break;
        }
    }
    mrm_t m;
    if (op < 0x40 && (op & 7) < 6) {
        mnem(d, alu[op >> 3]);
        switch (op & 7) {
        case 0: m = modrm(d); rm_op(d, &m, 0, 0); put(d, ",%s", reg8[m.reg]); break;
        case 1: m = modrm(d); rm_op(d, &m, 1, 0); put(d, ",%s", reg16[m.reg]); break;
        case 2: m = modrm(d); put(d, "%s,", reg8[m.reg]); rm_op(d, &m, 0, 0); break;
        case 3: m = modrm(d); put(d, "%s,", reg16[m.reg]); rm_op(d, &m, 1, 0); break;
        case 4: put(d, "AL,%02X", b8(d)); break;
        default: put(d, "AX,%04X", b16(d)); break;
        }
        return d->pos;
    }
    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E: mnem(d, "PUSH"); put(d, "%s", sreg[op >> 3]); break;
    case 0x07: case 0x17: case 0x1F: mnem(d, "POP"); put(d, "%s", sreg[op >> 3]); break;
    case 0x0F:
        if (!d->i286) {
            mnem(d, "POP");
            put(d, "CS");
            break;
        }
        op = b8(d);
        if (op == 0x01) {
            static const char *const g7[] = { "SGDT", "SIDT", "LGDT", "LIDT", "SMSW", "???", "LMSW", "???" };
            m = modrm(d);
            mnem(d, g7[m.reg]);
            rm_op(d, &m, 1, 0);
        } else if (op == 0x00) {
            static const char *const g6[] = { "SLDT", "STR", "LLDT", "LTR", "VERR", "VERW", "???", "???" };
            m = modrm(d);
            mnem(d, g6[m.reg]);
            rm_op(d, &m, 1, 0);
        } else if (op == 0x02 || op == 0x03) {
            m = modrm(d);
            mnem(d, op == 0x02 ? "LAR" : "LSL");
            put(d, "%s,", reg16[m.reg]);
            rm_op(d, &m, 1, 0);
        } else if (op == 0x05) {
            mnem(d, "LOADALL");
        } else if (op == 0x06) {
            mnem(d, "CLTS");
        } else {
            mnem(d, "DB");
            put(d, "0F,%02X", op);
        }
        break;
    case 0x27: mnem(d, "DAA"); break;
    case 0x2F: mnem(d, "DAS"); break;
    case 0x37: mnem(d, "AAA"); break;
    case 0x3F: mnem(d, "AAS"); break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        mnem(d, "INC"); put(d, "%s", reg16[op & 7]); break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        mnem(d, "DEC"); put(d, "%s", reg16[op & 7]); break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        mnem(d, "PUSH"); put(d, "%s", reg16[op & 7]); break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        mnem(d, "POP"); put(d, "%s", reg16[op & 7]); break;
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        if (!d->i286) {
            int8_t v = (int8_t)b8(d);
            mnem(d, jcc[op & 0x0F]);
            rel(d, d->pos, v);
            break;
        }
        switch (op) {
        case 0x60: mnem(d, "PUSHA"); break;
        case 0x61: mnem(d, "POPA"); break;
        case 0x62: m = modrm(d); mnem(d, "BOUND"); put(d, "%s,", reg16[m.reg]); rm_op(d, &m, 1, 0); break;
        case 0x63: m = modrm(d); mnem(d, "ARPL"); rm_op(d, &m, 1, 0); put(d, ",%s", reg16[m.reg]); break;
        case 0x68: mnem(d, "PUSH"); put(d, "%04X", b16(d)); break;
        case 0x6A: mnem(d, "PUSH"); put(d, "%02X", b8(d)); break;
        case 0x69: case 0x6B:
            m = modrm(d);
            mnem(d, "IMUL");
            put(d, "%s,", reg16[m.reg]);
            rm_op(d, &m, 1, 0);
            if (op == 0x69) {
                put(d, ",%04X", b16(d));
            } else {
                put(d, ",%02X", b8(d));
            }
            break;
        case 0x6C: put(d, "%s", rep ? rep : ""); mnem(d, "INSB"); break;
        case 0x6D: put(d, "%s", rep ? rep : ""); mnem(d, "INSW"); break;
        case 0x6E: put(d, "%s", rep ? rep : ""); mnem(d, "OUTSB"); break;
        case 0x6F: put(d, "%s", rep ? rep : ""); mnem(d, "OUTSW"); break;
        default: mnem(d, "DB"); put(d, "%02X", op); break;
        }
        break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t v = (int8_t)b8(d);
        mnem(d, jcc[op & 0x0F]);
        rel(d, d->pos, v);
        break;
    }
    case 0x80: case 0x82: m = modrm(d); mnem(d, alu[m.reg]); rm_op(d, &m, 0, 1); put(d, ",%02X", b8(d)); break;
    case 0x81: m = modrm(d); mnem(d, alu[m.reg]); rm_op(d, &m, 1, 2); put(d, ",%04X", b16(d)); break;
    case 0x83: {
        m = modrm(d);
        mnem(d, alu[m.reg]);
        rm_op(d, &m, 1, 2);
        int8_t v = (int8_t)b8(d);
        put(d, ",%c%02X", v < 0 ? '-' : '+', v < 0 ? -v : v);
        break;
    }
    case 0x84: m = modrm(d); mnem(d, "TEST"); rm_op(d, &m, 0, 0); put(d, ",%s", reg8[m.reg]); break;
    case 0x85: m = modrm(d); mnem(d, "TEST"); rm_op(d, &m, 1, 0); put(d, ",%s", reg16[m.reg]); break;
    case 0x86: m = modrm(d); mnem(d, "XCHG"); rm_op(d, &m, 0, 0); put(d, ",%s", reg8[m.reg]); break;
    case 0x87: m = modrm(d); mnem(d, "XCHG"); rm_op(d, &m, 1, 0); put(d, ",%s", reg16[m.reg]); break;
    case 0x88: m = modrm(d); mnem(d, "MOV"); rm_op(d, &m, 0, 0); put(d, ",%s", reg8[m.reg]); break;
    case 0x89: m = modrm(d); mnem(d, "MOV"); rm_op(d, &m, 1, 0); put(d, ",%s", reg16[m.reg]); break;
    case 0x8A: m = modrm(d); mnem(d, "MOV"); put(d, "%s,", reg8[m.reg]); rm_op(d, &m, 0, 0); break;
    case 0x8B: m = modrm(d); mnem(d, "MOV"); put(d, "%s,", reg16[m.reg]); rm_op(d, &m, 1, 0); break;
    case 0x8C: m = modrm(d); mnem(d, "MOV"); rm_op(d, &m, 1, 0); put(d, ",%s", sreg[m.reg & 3]); break;
    case 0x8D: m = modrm(d); mnem(d, "LEA"); put(d, "%s,", reg16[m.reg]); rm_op(d, &m, 1, 0); break;
    case 0x8E: m = modrm(d); mnem(d, "MOV"); put(d, "%s,", sreg[m.reg & 3]); rm_op(d, &m, 1, 0); break;
    case 0x8F: m = modrm(d); mnem(d, "POP"); rm_op(d, &m, 1, 2); break;
    case 0x90: mnem(d, "NOP"); break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        mnem(d, "XCHG"); put(d, "AX,%s", reg16[op & 7]); break;
    case 0x98: mnem(d, "CBW"); break;
    case 0x99: mnem(d, "CWD"); break;
    case 0x9A: {
        uint16_t o = b16(d);
        mnem(d, "CALL");
        put(d, "%04X:%04X", b16(d), o);
        break;
    }
    case 0x9B: mnem(d, "WAIT"); break;
    case 0x9C: mnem(d, "PUSHF"); break;
    case 0x9D: mnem(d, "POPF"); break;
    case 0x9E: mnem(d, "SAHF"); break;
    case 0x9F: mnem(d, "LAHF"); break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        uint16_t a = b16(d);
        const char *r = op & 1 ? "AX" : "AL";
        mnem(d, "MOV");
        if (op < 0xA2) {
            put(d, "%s,%s%s[%04X]", r, d->seg ? d->seg : "", d->seg ? ":" : "", a);
        } else {
            put(d, "%s%s[%04X],%s", d->seg ? d->seg : "", d->seg ? ":" : "", a, r);
        }
        break;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
        static const char *const s[] = { "MOVSB", "MOVSW", "CMPSB", "CMPSW", "", "", "STOSB", "STOSW",
                                         "LODSB", "LODSW", "SCASB", "SCASW" };
        if (rep) {
            put(d, "%s", (op & 0xF6) == 0xA6 || rep[3] == 'N' ? rep : "REP ");
        }
        mnem(d, s[op - 0xA4]);
        if (d->seg) {
            put(d, "%s:", d->seg);
        }
        break;
    }
    case 0xA8: mnem(d, "TEST"); put(d, "AL,%02X", b8(d)); break;
    case 0xA9: mnem(d, "TEST"); put(d, "AX,%04X", b16(d)); break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        mnem(d, "MOV"); put(d, "%s,%02X", reg8[op & 7], b8(d)); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        mnem(d, "MOV"); put(d, "%s,%04X", reg16[op & 7], b16(d)); break;
    case 0xC0: case 0xC1:
        if (d->i286) {
            m = modrm(d);
            mnem(d, m.reg == 6 ? "SAL" : shifts[m.reg]);
            rm_op(d, &m, op & 1, op & 1 ? 2 : 1);
            put(d, ",%02X", b8(d));
            break;
        }
        op += 2;
        /* fall through */
    case 0xC2: case 0xC3:
        mnem(d, "RET");
        if (op == 0xC2) {
            put(d, "%04X", b16(d));
        }
        break;
    case 0xC4: m = modrm(d); mnem(d, "LES"); put(d, "%s,", reg16[m.reg]); rm_op(d, &m, 1, 0); break;
    case 0xC5: m = modrm(d); mnem(d, "LDS"); put(d, "%s,", reg16[m.reg]); rm_op(d, &m, 1, 0); break;
    case 0xC6: m = modrm(d); mnem(d, "MOV"); rm_op(d, &m, 0, 1); put(d, ",%02X", b8(d)); break;
    case 0xC7: m = modrm(d); mnem(d, "MOV"); rm_op(d, &m, 1, 2); put(d, ",%04X", b16(d)); break;
    case 0xC8: case 0xC9:
        if (d->i286) {
            if (op == 0xC8) {
                uint16_t size = b16(d);
                mnem(d, "ENTER");
                put(d, "%04X,%02X", size, b8(d));
            } else {
                mnem(d, "LEAVE");
            }
            break;
        }
        op += 2;
        /* fall through */
    case 0xCA: case 0xCB:
        mnem(d, "RETF");
        if (op == 0xCA) {
            put(d, "%04X", b16(d));
        }
        break;
    case 0xCC: mnem(d, "INT"); put(d, "3"); break;
    case 0xCD: mnem(d, "INT"); put(d, "%02X", b8(d)); break;
    case 0xCE: mnem(d, "INTO"); break;
    case 0xCF: mnem(d, "IRET"); break;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        m = modrm(d);
        mnem(d, m.reg == 6 && d->i286 ? "SAL" : shifts[m.reg]);
        rm_op(d, &m, op & 1, op & 1 ? 2 : 1);
        put(d, op < 0xD2 ? ",1" : ",CL");
        break;
    case 0xD4: mnem(d, "AAM"); { uint8_t v = b8(d); if (v != 10) put(d, "%02X", v); } break;
    case 0xD5: mnem(d, "AAD"); { uint8_t v = b8(d); if (v != 10) put(d, "%02X", v); } break;
    case 0xD6: mnem(d, "SALC"); break;
    case 0xD7: mnem(d, "XLAT"); break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        esc(d, op);
        break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        static const char *const l[] = { "LOOPNZ", "LOOPZ", "LOOP", "JCXZ" };
        int8_t v = (int8_t)b8(d);
        mnem(d, l[op & 3]);
        rel(d, d->pos, v);
        break;
    }
    case 0xE4: mnem(d, "IN"); put(d, "AL,%02X", b8(d)); break;
    case 0xE5: mnem(d, "IN"); put(d, "AX,%02X", b8(d)); break;
    case 0xE6: mnem(d, "OUT"); put(d, "%02X,AL", b8(d)); break;
    case 0xE7: mnem(d, "OUT"); put(d, "%02X,AX", b8(d)); break;
    case 0xE8: { uint16_t v = b16(d); mnem(d, "CALL"); rel(d, d->pos, (int16_t)v); break; }
    case 0xE9: { uint16_t v = b16(d); mnem(d, "JMP"); rel(d, d->pos, (int16_t)v); break; }
    case 0xEA: {
        uint16_t o = b16(d);
        mnem(d, "JMP");
        put(d, "%04X:%04X", b16(d), o);
        break;
    }
    case 0xEB: { int8_t v = (int8_t)b8(d); mnem(d, "JMP"); rel(d, d->pos, v); break; }
    case 0xEC: mnem(d, "IN"); put(d, "AL,DX"); break;
    case 0xED: mnem(d, "IN"); put(d, "AX,DX"); break;
    case 0xEE: mnem(d, "OUT"); put(d, "DX,AL"); break;
    case 0xEF: mnem(d, "OUT"); put(d, "DX,AX"); break;
    case 0xF4: mnem(d, "HLT"); break;
    case 0xF5: mnem(d, "CMC"); break;
    case 0xF6: case 0xF7:
        m = modrm(d);
        mnem(d, grp3[m.reg]);
        rm_op(d, &m, op & 1, op & 1 ? 2 : 1);
        if (m.reg < 2) {
            if (op & 1) {
                put(d, ",%04X", b16(d));
            } else {
                put(d, ",%02X", b8(d));
            }
        }
        break;
    case 0xF8: mnem(d, "CLC"); break;
    case 0xF9: mnem(d, "STC"); break;
    case 0xFA: mnem(d, "CLI"); break;
    case 0xFB: mnem(d, "STI"); break;
    case 0xFC: mnem(d, "CLD"); break;
    case 0xFD: mnem(d, "STD"); break;
    case 0xFE:
        if (d->pos < d->max && d->bytes[d->pos] == 0x38) {
            d->pos++;
            mnem(d, "TRAP");            /* the BIOS's (only inside its ROM) */
            put(d, "%02X", b8(d));
            break;
        }
        m = modrm(d);
        mnem(d, m.reg == 0 ? "INC" : m.reg == 1 ? "DEC" : "???");
        rm_op(d, &m, 0, 1);
        break;
    case 0xFF:
        m = modrm(d);
        switch (m.reg) {
        case 0: mnem(d, "INC"); rm_op(d, &m, 1, 2); break;
        case 1: mnem(d, "DEC"); rm_op(d, &m, 1, 2); break;
        case 2: mnem(d, "CALL"); rm_op(d, &m, 1, 0); break;
        case 3: mnem(d, "CALL"); put(d, "FAR "); rm_op(d, &m, 1, 0); break;
        case 4: mnem(d, "JMP"); rm_op(d, &m, 1, 0); break;
        case 5: mnem(d, "JMP"); put(d, "FAR "); rm_op(d, &m, 1, 0); break;
        default: mnem(d, "PUSH"); rm_op(d, &m, 1, 2); break;
        }
        break;
    default:
        mnem(d, "DB");
        put(d, "%02X", op);
        break;
    }
    /* Trailing spaces of a bare mnemonic */
    while (d->n > 0 && d->out[d->n - 1] == ' ') {
        d->out[--d->n] = 0;
    }
    return d->pos;
}

int x86_disassemble_bytes(x86_model_t model, const uint8_t *bytes, uint16_t off, char *buf, int len)
{
    dis_t d = { .bytes = bytes, .max = 16, .off = off, .i286 = model == X86_80286, .out = buf, .len = len };
    if (len > 0) {
        buf[0] = 0;
    }
    return decode(&d);
}

int x86_disassemble(const x86_t *cpu, uint16_t seg, uint16_t off, char *buf, int len)
{
    uint8_t bytes[16];
    uint32_t base = (uint32_t)seg << 4;
    for (int i = 0; i < 16; i++) {
        bytes[i] = x86_peek8(cpu, base + (uint16_t)(off + i));
    }
    return x86_disassemble_bytes(cpu->model, bytes, off, buf, len);
}
