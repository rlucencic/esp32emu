/*
 * 6809 / 6309 disassembler: Motorola syntax, upper case, $ hex. The 6309's
 * instructions only for a core with h6309 set.
 */
#include <stdio.h>
#include <string.h>
#include "m6809.h"

enum { INH, IMM8, IMM16, IMM32, DIR, IDX, EXT, REL8, REL16, R_R, PSH_S, PSH_U, IMMDIR, IMMIDX, IMMEXT, BITDIR, TFM_M, ILL };

typedef struct {
    const char *name;
    uint8_t mode;
    uint8_t h6309;      /* the 6309's only */
} op_t;

#define O(n, m)     { n, m, 0 }
#define H(n, m)     { n, m, 1 }
#define X           { "", ILL, 0 }

static const op_t page0[256] = {
    /* $00 */ O("NEG", DIR), H("OIM", IMMDIR), H("AIM", IMMDIR), O("COM", DIR), O("LSR", DIR), H("EIM", IMMDIR), O("ROR", DIR), O("ASR", DIR),
              O("ASL", DIR), O("ROL", DIR), O("DEC", DIR), H("TIM", IMMDIR), O("INC", DIR), O("TST", DIR), O("JMP", DIR), O("CLR", DIR),
    /* $10 */ X, X, O("NOP", INH), O("SYNC", INH), H("SEXW", INH), X, O("LBRA", REL16), O("LBSR", REL16),
              X, O("DAA", INH), O("ORCC", IMM8), X, O("ANDCC", IMM8), O("SEX", INH), O("EXG", R_R), O("TFR", R_R),
    /* $20 */ O("BRA", REL8), O("BRN", REL8), O("BHI", REL8), O("BLS", REL8), O("BCC", REL8), O("BCS", REL8), O("BNE", REL8), O("BEQ", REL8),
              O("BVC", REL8), O("BVS", REL8), O("BPL", REL8), O("BMI", REL8), O("BGE", REL8), O("BLT", REL8), O("BGT", REL8), O("BLE", REL8),
    /* $30 */ O("LEAX", IDX), O("LEAY", IDX), O("LEAS", IDX), O("LEAU", IDX), O("PSHS", PSH_S), O("PULS", PSH_S), O("PSHU", PSH_U), O("PULU", PSH_U),
              X, O("RTS", INH), O("ABX", INH), O("RTI", INH), O("CWAI", IMM8), O("MUL", INH), X, O("SWI", INH),
    /* $40 */ O("NEGA", INH), X, X, O("COMA", INH), O("LSRA", INH), X, O("RORA", INH), O("ASRA", INH),
              O("ASLA", INH), O("ROLA", INH), O("DECA", INH), X, O("INCA", INH), O("TSTA", INH), X, O("CLRA", INH),
    /* $50 */ O("NEGB", INH), X, X, O("COMB", INH), O("LSRB", INH), X, O("RORB", INH), O("ASRB", INH),
              O("ASLB", INH), O("ROLB", INH), O("DECB", INH), X, O("INCB", INH), O("TSTB", INH), X, O("CLRB", INH),
    /* $60 */ O("NEG", IDX), H("OIM", IMMIDX), H("AIM", IMMIDX), O("COM", IDX), O("LSR", IDX), H("EIM", IMMIDX), O("ROR", IDX), O("ASR", IDX),
              O("ASL", IDX), O("ROL", IDX), O("DEC", IDX), H("TIM", IMMIDX), O("INC", IDX), O("TST", IDX), O("JMP", IDX), O("CLR", IDX),
    /* $70 */ O("NEG", EXT), H("OIM", IMMEXT), H("AIM", IMMEXT), O("COM", EXT), O("LSR", EXT), H("EIM", IMMEXT), O("ROR", EXT), O("ASR", EXT),
              O("ASL", EXT), O("ROL", EXT), O("DEC", EXT), H("TIM", IMMEXT), O("INC", EXT), O("TST", EXT), O("JMP", EXT), O("CLR", EXT),
    /* $80 */ O("SUBA", IMM8), O("CMPA", IMM8), O("SBCA", IMM8), O("SUBD", IMM16), O("ANDA", IMM8), O("BITA", IMM8), O("LDA", IMM8), X,
              O("EORA", IMM8), O("ADCA", IMM8), O("ORA", IMM8), O("ADDA", IMM8), O("CMPX", IMM16), O("BSR", REL8), O("LDX", IMM16), X,
    /* $90 */ O("SUBA", DIR), O("CMPA", DIR), O("SBCA", DIR), O("SUBD", DIR), O("ANDA", DIR), O("BITA", DIR), O("LDA", DIR), O("STA", DIR),
              O("EORA", DIR), O("ADCA", DIR), O("ORA", DIR), O("ADDA", DIR), O("CMPX", DIR), O("JSR", DIR), O("LDX", DIR), O("STX", DIR),
    /* $A0 */ O("SUBA", IDX), O("CMPA", IDX), O("SBCA", IDX), O("SUBD", IDX), O("ANDA", IDX), O("BITA", IDX), O("LDA", IDX), O("STA", IDX),
              O("EORA", IDX), O("ADCA", IDX), O("ORA", IDX), O("ADDA", IDX), O("CMPX", IDX), O("JSR", IDX), O("LDX", IDX), O("STX", IDX),
    /* $B0 */ O("SUBA", EXT), O("CMPA", EXT), O("SBCA", EXT), O("SUBD", EXT), O("ANDA", EXT), O("BITA", EXT), O("LDA", EXT), O("STA", EXT),
              O("EORA", EXT), O("ADCA", EXT), O("ORA", EXT), O("ADDA", EXT), O("CMPX", EXT), O("JSR", EXT), O("LDX", EXT), O("STX", EXT),
    /* $C0 */ O("SUBB", IMM8), O("CMPB", IMM8), O("SBCB", IMM8), O("ADDD", IMM16), O("ANDB", IMM8), O("BITB", IMM8), O("LDB", IMM8), X,
              O("EORB", IMM8), O("ADCB", IMM8), O("ORB", IMM8), O("ADDB", IMM8), O("LDD", IMM16), H("LDQ", IMM32), O("LDU", IMM16), X,
    /* $D0 */ O("SUBB", DIR), O("CMPB", DIR), O("SBCB", DIR), O("ADDD", DIR), O("ANDB", DIR), O("BITB", DIR), O("LDB", DIR), O("STB", DIR),
              O("EORB", DIR), O("ADCB", DIR), O("ORB", DIR), O("ADDB", DIR), O("LDD", DIR), O("STD", DIR), O("LDU", DIR), O("STU", DIR),
    /* $E0 */ O("SUBB", IDX), O("CMPB", IDX), O("SBCB", IDX), O("ADDD", IDX), O("ANDB", IDX), O("BITB", IDX), O("LDB", IDX), O("STB", IDX),
              O("EORB", IDX), O("ADCB", IDX), O("ORB", IDX), O("ADDB", IDX), O("LDD", IDX), O("STD", IDX), O("LDU", IDX), O("STU", IDX),
    /* $F0 */ O("SUBB", EXT), O("CMPB", EXT), O("SBCB", EXT), O("ADDD", EXT), O("ANDB", EXT), O("BITB", EXT), O("LDB", EXT), O("STB", EXT),
              O("EORB", EXT), O("ADCB", EXT), O("ORB", EXT), O("ADDB", EXT), O("LDD", EXT), O("STD", EXT), O("LDU", EXT), O("STU", EXT),
};

typedef struct {
    uint8_t op;
    op_t o;
} op2_t;

static const op2_t page2[] = {
    { 0x21, O("LBRN", REL16) }, { 0x22, O("LBHI", REL16) }, { 0x23, O("LBLS", REL16) }, { 0x24, O("LBCC", REL16) },
    { 0x25, O("LBCS", REL16) }, { 0x26, O("LBNE", REL16) }, { 0x27, O("LBEQ", REL16) }, { 0x28, O("LBVC", REL16) },
    { 0x29, O("LBVS", REL16) }, { 0x2A, O("LBPL", REL16) }, { 0x2B, O("LBMI", REL16) }, { 0x2C, O("LBGE", REL16) },
    { 0x2D, O("LBLT", REL16) }, { 0x2E, O("LBGT", REL16) }, { 0x2F, O("LBLE", REL16) },
    { 0x30, H("ADDR", R_R) }, { 0x31, H("ADCR", R_R) }, { 0x32, H("SUBR", R_R) }, { 0x33, H("SBCR", R_R) },
    { 0x34, H("ANDR", R_R) }, { 0x35, H("ORR", R_R) }, { 0x36, H("EORR", R_R) }, { 0x37, H("CMPR", R_R) },
    { 0x38, H("PSHSW", INH) }, { 0x39, H("PULSW", INH) }, { 0x3A, H("PSHUW", INH) }, { 0x3B, H("PULUW", INH) },
    { 0x3F, O("SWI2", INH) },
    { 0x40, H("NEGD", INH) }, { 0x43, H("COMD", INH) }, { 0x44, H("LSRD", INH) }, { 0x46, H("RORD", INH) },
    { 0x47, H("ASRD", INH) }, { 0x48, H("ASLD", INH) }, { 0x49, H("ROLD", INH) }, { 0x4A, H("DECD", INH) },
    { 0x4C, H("INCD", INH) }, { 0x4D, H("TSTD", INH) }, { 0x4F, H("CLRD", INH) },
    { 0x53, H("COMW", INH) }, { 0x54, H("LSRW", INH) }, { 0x56, H("RORW", INH) }, { 0x59, H("ROLW", INH) },
    { 0x5A, H("DECW", INH) }, { 0x5C, H("INCW", INH) }, { 0x5D, H("TSTW", INH) }, { 0x5F, H("CLRW", INH) },
    { 0x80, H("SUBW", IMM16) }, { 0x81, H("CMPW", IMM16) }, { 0x82, H("SBCD", IMM16) }, { 0x83, O("CMPD", IMM16) },
    { 0x84, H("ANDD", IMM16) }, { 0x85, H("BITD", IMM16) }, { 0x86, H("LDW", IMM16) }, { 0x88, H("EORD", IMM16) },
    { 0x89, H("ADCD", IMM16) }, { 0x8A, H("ORD", IMM16) }, { 0x8B, H("ADDW", IMM16) }, { 0x8C, O("CMPY", IMM16) },
    { 0x8E, O("LDY", IMM16) },
    { 0x90, H("SUBW", DIR) }, { 0x91, H("CMPW", DIR) }, { 0x92, H("SBCD", DIR) }, { 0x93, O("CMPD", DIR) },
    { 0x94, H("ANDD", DIR) }, { 0x95, H("BITD", DIR) }, { 0x96, H("LDW", DIR) }, { 0x97, H("STW", DIR) },
    { 0x98, H("EORD", DIR) }, { 0x99, H("ADCD", DIR) }, { 0x9A, H("ORD", DIR) }, { 0x9B, H("ADDW", DIR) },
    { 0x9C, O("CMPY", DIR) }, { 0x9E, O("LDY", DIR) }, { 0x9F, O("STY", DIR) },
    { 0xA0, H("SUBW", IDX) }, { 0xA1, H("CMPW", IDX) }, { 0xA2, H("SBCD", IDX) }, { 0xA3, O("CMPD", IDX) },
    { 0xA4, H("ANDD", IDX) }, { 0xA5, H("BITD", IDX) }, { 0xA6, H("LDW", IDX) }, { 0xA7, H("STW", IDX) },
    { 0xA8, H("EORD", IDX) }, { 0xA9, H("ADCD", IDX) }, { 0xAA, H("ORD", IDX) }, { 0xAB, H("ADDW", IDX) },
    { 0xAC, O("CMPY", IDX) }, { 0xAE, O("LDY", IDX) }, { 0xAF, O("STY", IDX) },
    { 0xB0, H("SUBW", EXT) }, { 0xB1, H("CMPW", EXT) }, { 0xB2, H("SBCD", EXT) }, { 0xB3, O("CMPD", EXT) },
    { 0xB4, H("ANDD", EXT) }, { 0xB5, H("BITD", EXT) }, { 0xB6, H("LDW", EXT) }, { 0xB7, H("STW", EXT) },
    { 0xB8, H("EORD", EXT) }, { 0xB9, H("ADCD", EXT) }, { 0xBA, H("ORD", EXT) }, { 0xBB, H("ADDW", EXT) },
    { 0xBC, O("CMPY", EXT) }, { 0xBE, O("LDY", EXT) }, { 0xBF, O("STY", EXT) },
    { 0xCE, O("LDS", IMM16) }, { 0xDC, H("LDQ", DIR) }, { 0xDD, H("STQ", DIR) }, { 0xDE, O("LDS", DIR) },
    { 0xDF, O("STS", DIR) }, { 0xEC, H("LDQ", IDX) }, { 0xED, H("STQ", IDX) }, { 0xEE, O("LDS", IDX) },
    { 0xEF, O("STS", IDX) }, { 0xFC, H("LDQ", EXT) }, { 0xFD, H("STQ", EXT) }, { 0xFE, O("LDS", EXT) },
    { 0xFF, O("STS", EXT) },
};

static const op2_t page3[] = {
    { 0x30, H("BAND", BITDIR) }, { 0x31, H("BIAND", BITDIR) }, { 0x32, H("BOR", BITDIR) }, { 0x33, H("BIOR", BITDIR) },
    { 0x34, H("BEOR", BITDIR) }, { 0x35, H("BIEOR", BITDIR) }, { 0x36, H("LDBT", BITDIR) }, { 0x37, H("STBT", BITDIR) },
    { 0x38, H("TFM", TFM_M) }, { 0x39, H("TFM", TFM_M) }, { 0x3A, H("TFM", TFM_M) }, { 0x3B, H("TFM", TFM_M) },
    { 0x3C, H("BITMD", IMM8) }, { 0x3D, H("LDMD", IMM8) }, { 0x3F, O("SWI3", INH) },
    { 0x43, H("COME", INH) }, { 0x4A, H("DECE", INH) }, { 0x4C, H("INCE", INH) }, { 0x4D, H("TSTE", INH) }, { 0x4F, H("CLRE", INH) },
    { 0x53, H("COMF", INH) }, { 0x5A, H("DECF", INH) }, { 0x5C, H("INCF", INH) }, { 0x5D, H("TSTF", INH) }, { 0x5F, H("CLRF", INH) },
    { 0x80, H("SUBE", IMM8) }, { 0x81, H("CMPE", IMM8) }, { 0x83, O("CMPU", IMM16) }, { 0x86, H("LDE", IMM8) },
    { 0x8B, H("ADDE", IMM8) }, { 0x8C, O("CMPS", IMM16) }, { 0x8D, H("DIVD", IMM8) }, { 0x8E, H("DIVQ", IMM16) },
    { 0x8F, H("MULD", IMM16) },
    { 0x90, H("SUBE", DIR) }, { 0x91, H("CMPE", DIR) }, { 0x93, O("CMPU", DIR) }, { 0x96, H("LDE", DIR) },
    { 0x97, H("STE", DIR) }, { 0x9B, H("ADDE", DIR) }, { 0x9C, O("CMPS", DIR) }, { 0x9D, H("DIVD", DIR) },
    { 0x9E, H("DIVQ", DIR) }, { 0x9F, H("MULD", DIR) },
    { 0xA0, H("SUBE", IDX) }, { 0xA1, H("CMPE", IDX) }, { 0xA3, O("CMPU", IDX) }, { 0xA6, H("LDE", IDX) },
    { 0xA7, H("STE", IDX) }, { 0xAB, H("ADDE", IDX) }, { 0xAC, O("CMPS", IDX) }, { 0xAD, H("DIVD", IDX) },
    { 0xAE, H("DIVQ", IDX) }, { 0xAF, H("MULD", IDX) },
    { 0xB0, H("SUBE", EXT) }, { 0xB1, H("CMPE", EXT) }, { 0xB3, O("CMPU", EXT) }, { 0xB6, H("LDE", EXT) },
    { 0xB7, H("STE", EXT) }, { 0xBB, H("ADDE", EXT) }, { 0xBC, O("CMPS", EXT) }, { 0xBD, H("DIVD", EXT) },
    { 0xBE, H("DIVQ", EXT) }, { 0xBF, H("MULD", EXT) },
    { 0xC0, H("SUBF", IMM8) }, { 0xC1, H("CMPF", IMM8) }, { 0xC6, H("LDF", IMM8) }, { 0xCB, H("ADDF", IMM8) },
    { 0xD0, H("SUBF", DIR) }, { 0xD1, H("CMPF", DIR) }, { 0xD6, H("LDF", DIR) }, { 0xD7, H("STF", DIR) }, { 0xDB, H("ADDF", DIR) },
    { 0xE0, H("SUBF", IDX) }, { 0xE1, H("CMPF", IDX) }, { 0xE6, H("LDF", IDX) }, { 0xE7, H("STF", IDX) }, { 0xEB, H("ADDF", IDX) },
    { 0xF0, H("SUBF", EXT) }, { 0xF1, H("CMPF", EXT) }, { 0xF6, H("LDF", EXT) }, { 0xF7, H("STF", EXT) }, { 0xFB, H("ADDF", EXT) },
};

static const op_t *find(const op2_t *table, int n, uint8_t op)
{
    for (int i = 0; i < n; i++) {
        if (table[i].op == op) {
            return &table[i].o;
        }
    }
    return NULL;
}

static const char *const reg_names[16] = { "D", "X", "Y", "U", "S", "PC", "W", "V", "A", "B", "CC", "DP", "0", "0", "E", "F" };

/* The indexed operand at addr into out; bytes it takes */
static int indexed(const m6809_t *c, uint16_t addr, char *out, size_t len)
{
    static const char *const r[4] = { "X", "Y", "U", "S" };
    uint8_t post = m6809_peek(c, addr);
    const char *R = r[(post >> 5) & 3];
    if (!(post & 0x80)) {
        int off = (int8_t)(post << 3) >> 3;
        snprintf(out, len, "%d,%s", off, R);
        return 1;
    }
    bool ind = post & 0x10;
    char body[32];
    int n = 1;
    switch (post & 0x0F) {
    case 0x0:
        if (ind && c->h6309) {
            static const char *const w[4] = { ",W", "", ",W++", ",--W" };
            if ((post & 0x60) == 0x20) {
                uint16_t off = (uint16_t)(m6809_peek(c, addr + 1) << 8 | m6809_peek(c, addr + 2));
                snprintf(out, len, "[$%04X,W]", off);
                return 3;
            }
            snprintf(out, len, "[%s]", w[(post >> 5) & 3]);
            return 1;
        }
        snprintf(body, sizeof(body), ",%s+", R);
        break;
    case 0x1: snprintf(body, sizeof(body), ",%s++", R); break;
    case 0x2: snprintf(body, sizeof(body), ",-%s", R); break;
    case 0x3: snprintf(body, sizeof(body), ",--%s", R); break;
    case 0x4: snprintf(body, sizeof(body), ",%s", R); break;
    case 0x5: snprintf(body, sizeof(body), "B,%s", R); break;
    case 0x6: snprintf(body, sizeof(body), "A,%s", R); break;
    case 0x7: snprintf(body, sizeof(body), "E,%s", R); break;
    case 0x8: snprintf(body, sizeof(body), "%d,%s", (int8_t)m6809_peek(c, addr + 1), R); n = 2; break;
    case 0x9:
        snprintf(body, sizeof(body), "$%04X,%s", m6809_peek(c, addr + 1) << 8 | m6809_peek(c, addr + 2), R);
        n = 3;
        break;
    case 0xA: snprintf(body, sizeof(body), "F,%s", R); break;
    case 0xB: snprintf(body, sizeof(body), "D,%s", R); break;
    case 0xC: {
        int8_t off = (int8_t)m6809_peek(c, addr + 1);
        snprintf(body, sizeof(body), "$%04X,PCR", (uint16_t)(addr + 2 + off));
        n = 2;
        break;
    }
    case 0xD: {
        uint16_t off = (uint16_t)(m6809_peek(c, addr + 1) << 8 | m6809_peek(c, addr + 2));
        snprintf(body, sizeof(body), "$%04X,PCR", (uint16_t)(addr + 3 + off));
        n = 3;
        break;
    }
    case 0xE: snprintf(body, sizeof(body), "W,%s", R); break;
    default:
        if (ind) {
            snprintf(body, sizeof(body), "$%04X", m6809_peek(c, addr + 1) << 8 | m6809_peek(c, addr + 2));
            n = 3;
            break;
        }
        switch (post & 0x60) {
        case 0x00: snprintf(body, sizeof(body), ",W"); break;
        case 0x20:
            snprintf(body, sizeof(body), "$%04X,W", m6809_peek(c, addr + 1) << 8 | m6809_peek(c, addr + 2));
            n = 3;
            break;
        case 0x40: snprintf(body, sizeof(body), ",W++"); break;
        default: snprintf(body, sizeof(body), ",--W"); break;
        }
        break;
    }
    if (ind) {
        snprintf(out, len, "[%s]", body);
    } else {
        snprintf(out, len, "%s", body);
    }
    return n;
}

static void reg_list(uint8_t mask, bool user, char *out, size_t len)
{
    static const char *const s_names[8] = { "CC", "A", "B", "DP", "X", "Y", "U", "PC" };
    out[0] = 0;
    size_t at = 0;
    for (int i = 0; i < 8; i++) {
        if (mask & (1 << i)) {
            const char *n = (i == 6 && user) ? "S" : s_names[i];
            at += (size_t)snprintf(out + at, at < len ? len - at : 0, "%s%s", at ? "," : "", n);
        }
    }
}

static int decode(const m6809_t *c, uint16_t addr, char *buf, int len)
{
    uint16_t start = addr;
    uint8_t op = m6809_peek(c, addr++);
    const op_t *o;
    char prefix_hex[8] = "";
    if (op == 0x10 || op == 0x11) {
        uint8_t op2 = m6809_peek(c, addr++);
        o = op == 0x10 ? find(page2, sizeof(page2) / sizeof(page2[0]), op2)
                       : find(page3, sizeof(page3) / sizeof(page3[0]), op2);
        if (o == NULL || (o->h6309 && !c->h6309)) {
            if (buf) {
                snprintf(buf, (size_t)len, "FCB $%02X,$%02X", op, op2);
            }
            return 2;
        }
        snprintf(prefix_hex, sizeof(prefix_hex), "%02X", op2);
    } else {
        o = &page0[op];
        if (o->mode == ILL || (o->h6309 && !c->h6309)) {
            if (buf) {
                snprintf(buf, (size_t)len, "FCB $%02X", op);
            }
            return 1;
        }
    }
    (void)prefix_hex;
    char arg[48] = "";
    switch (o->mode) {
    case INH: break;
    case IMM8: snprintf(arg, sizeof(arg), "#$%02X", m6809_peek(c, addr)); addr += 1; break;
    case IMM16: snprintf(arg, sizeof(arg), "#$%04X", m6809_peek(c, addr) << 8 | m6809_peek(c, addr + 1)); addr += 2; break;
    case IMM32:
        snprintf(arg, sizeof(arg), "#$%02X%02X%02X%02X", m6809_peek(c, addr), m6809_peek(c, addr + 1),
                 m6809_peek(c, addr + 2), m6809_peek(c, addr + 3));
        addr += 4;
        break;
    case DIR: snprintf(arg, sizeof(arg), "<$%02X", m6809_peek(c, addr)); addr += 1; break;
    case EXT: snprintf(arg, sizeof(arg), "$%04X", m6809_peek(c, addr) << 8 | m6809_peek(c, addr + 1)); addr += 2; break;
    case IDX: addr += (uint16_t)indexed(c, addr, arg, sizeof(arg)); break;
    case REL8: {
        int8_t off = (int8_t)m6809_peek(c, addr);
        addr += 1;
        snprintf(arg, sizeof(arg), "$%04X", (uint16_t)(addr + off));
        break;
    }
    case REL16: {
        uint16_t off = (uint16_t)(m6809_peek(c, addr) << 8 | m6809_peek(c, addr + 1));
        addr += 2;
        snprintf(arg, sizeof(arg), "$%04X", (uint16_t)(addr + off));
        break;
    }
    case R_R: {
        uint8_t post = m6809_peek(c, addr++);
        snprintf(arg, sizeof(arg), "%s,%s", reg_names[post >> 4], reg_names[post & 15]);
        break;
    }
    case PSH_S: case PSH_U:
        reg_list(m6809_peek(c, addr++), o->mode == PSH_U, arg, sizeof(arg));
        break;
    case IMMDIR: case IMMIDX: case IMMEXT: {
        uint8_t im = m6809_peek(c, addr++);
        char ea[40];
        if (o->mode == IMMDIR) {
            snprintf(ea, sizeof(ea), "<$%02X", m6809_peek(c, addr));
            addr += 1;
        } else if (o->mode == IMMEXT) {
            snprintf(ea, sizeof(ea), "$%04X", m6809_peek(c, addr) << 8 | m6809_peek(c, addr + 1));
            addr += 2;
        } else {
            addr += (uint16_t)indexed(c, addr, ea, sizeof(ea));
        }
        snprintf(arg, sizeof(arg), "#$%02X,%s", im, ea);
        break;
    }
    case BITDIR: {
        static const char *const r[4] = { "CC", "A", "B", "?" };
        uint8_t post = m6809_peek(c, addr++);
        uint8_t d = m6809_peek(c, addr++);
        snprintf(arg, sizeof(arg), "%s.%d,<$%02X.%d", r[post >> 6], post & 7, d, (post >> 3) & 7);
        break;
    }
    case TFM_M: {
        static const char *const fmt[4] = { "%s+,%s+", "%s-,%s-", "%s+,%s", "%s,%s+" };
        uint8_t post = m6809_peek(c, addr++);
        uint8_t op2 = m6809_peek(c, (uint16_t)(start + 1));
        snprintf(arg, sizeof(arg), fmt[op2 & 3], reg_names[post >> 4], reg_names[post & 15]);
        break;
    }
    default: break;
    }
    if (buf) {
        if (arg[0]) {
            snprintf(buf, (size_t)len, "%-5s %s", o->name, arg);
        } else {
            snprintf(buf, (size_t)len, "%s", o->name);
        }
    }
    return (uint16_t)(addr - start);
}

int m6809_disassemble(const m6809_t *c, uint16_t addr, char *buf, int len)
{
    return decode(c, addr, buf, len);
}

int m6809_insn_length(const m6809_t *c, uint16_t addr)
{
    return decode(c, addr, NULL, 0);
}
