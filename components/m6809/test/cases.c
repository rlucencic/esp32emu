/*
 * 6809 / 6309 instruction cases, from the MC6809E data sheet and the
 * HD6309 Technical Reference: code at $1000 (unless PC= says otherwise),
 * registers and memory before, then after N steps the registers, memory
 * and cycles expected. Flat 64 KB, the vectors from the case's memory.
 *
 *   cases [-v]      (-v: every case, not just the failures)
 *
 * Register text: "A=12 B=34 D=1234 X=.. Y=.. U=.. S=.. PC=.. DP=.. CC=..
 * E=.. F=.. W=.. V=.. MD=.."; memory "M1234=AB CD": bytes from $1234.
 * "IRQ" / "FIRQ" / "NMI" assert the line before the steps.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m6809.h"

typedef struct {
    const char *name;
    int cpu;                    /* 0 6809, 1 6309 (emulation), 2 6309 native */
    const char *code;           /* hex bytes at PC */
    const char *before;
    int steps;
    const char *after;
    int cycles;                 /* -1: don't check */
} test_case_t;

/* CC bits: E80 F40 H20 I10 N08 Z04 V02 C01 */
static const test_case_t cases[] = {
    /* ---- 8-bit arithmetic ---- */
    { "ADDA half carry, carry, zero", 0, "8B 08", "A=F8 CC=00", 1, "A=00 CC=25", 2 },
    { "ADDA overflow", 0, "8B 01", "A=7F CC=00", 1, "A=80 CC=2A", 2 },
    { "ADDA no flags", 0, "8B 11", "A=22 CC=00", 1, "A=33 CC=00", 2 },
    { "ADCA with carry", 0, "89 00", "A=FF CC=01", 1, "A=00 CC=25", 2 },
    { "ADDB direct", 0, "DB 10", "B=10 DP=20 M2010=20 CC=00", 1, "B=30 CC=00", 4 },
    { "SUBA borrow", 0, "80 01", "A=00 CC=00", 1, "A=FF CC=09", 2 },
    { "SUBA overflow", 0, "80 01", "A=80 CC=00", 1, "A=7F CC=02", 2 },
    { "SBCA with borrow", 0, "82 00", "A=00 CC=01", 1, "A=FF CC=09", 2 },
    { "CMPA equal", 0, "81 42", "A=42 CC=00", 1, "A=42 CC=04", 2 },
    { "CMPB less (unsigned)", 0, "C1 80", "B=7F CC=00", 1, "B=7F CC=0B", 2 },
    { "ANDA clears V keeps C", 0, "84 0F", "A=F3 CC=03", 1, "A=03 CC=01", 2 },
    { "ORA negative", 0, "8A 80", "A=01 CC=00", 1, "A=81 CC=08", 2 },
    { "EORB zero", 0, "C8 55", "B=55 CC=02", 1, "B=00 CC=04", 2 },
    { "BITA", 0, "85 80", "A=80 CC=00", 1, "A=80 CC=08", 2 },
    { "LDA zero", 0, "86 00", "A=55 CC=0B", 1, "A=00 CC=05", 2 },
    { "LDB extended", 0, "F6 20 00", "M2000=80 CC=00", 1, "B=80 CC=08", 5 },
    { "STA direct", 0, "97 40", "A=99 DP=12 CC=00", 1, "M1240=99 CC=08", 4 },
    { "DAA 09+08 (H stays)", 0, "8B 08 19", "A=09 CC=00", 2, "A=17 CC=20", 4 },
    { "DAA 99+01", 0, "8B 01 19", "A=99 CC=00", 2, "A=00 CC=05", 4 },
    { "MUL", 0, "3D", "A=12 B=34 CC=00", 1, "D=03A8 CC=01", 11 },
    { "MUL zero", 0, "3D", "A=00 B=34 CC=01", 1, "D=0000 CC=04", 11 },
    { "SEX negative", 0, "1D", "A=00 B=80 CC=00", 1, "D=FF80 CC=08", 2 },
    { "ABX unsigned", 0, "3A", "X=1000 B=FF", 1, "X=10FF", 3 },
    /* ---- read-modify-write ---- */
    { "NEGA 80", 0, "40", "A=80 CC=00", 1, "A=80 CC=0B", 2 },
    { "NEGA 00", 0, "40", "A=00 CC=01", 1, "A=00 CC=04", 2 },
    { "NEGA 01", 0, "40", "A=01 CC=00", 1, "A=FF CC=09", 2 },
    { "COMA", 0, "43", "A=0F CC=02", 1, "A=F0 CC=09", 2 },
    { "LSRA", 0, "44", "A=81 CC=08", 1, "A=40 CC=01", 2 },
    { "LSRA to zero", 0, "44", "A=01 CC=00", 1, "A=00 CC=05", 2 },
    { "RORA carry in", 0, "46", "A=02 CC=01", 1, "A=81 CC=08", 2 },
    { "ASRA", 0, "47", "A=81 CC=00", 1, "A=C0 CC=09", 2 },
    { "ASLA overflow", 0, "48", "A=40 CC=00", 1, "A=80 CC=0A", 2 },
    { "ASLA carry", 0, "48", "A=C0 CC=00", 1, "A=80 CC=09", 2 },
    { "ROLA", 0, "49", "A=80 CC=01", 1, "A=01 CC=03", 2 },
    { "DECA overflow", 0, "4A", "A=80 CC=01", 1, "A=7F CC=03", 2 },
    { "INCA overflow", 0, "4C", "A=7F CC=00", 1, "A=80 CC=0A", 2 },
    { "TSTB", 0, "5D", "B=00 CC=03", 1, "B=00 CC=05", 2 },
    { "CLRB", 0, "5F", "B=55 CC=0B", 1, "B=00 CC=04", 2 },
    { "INC direct", 0, "0C 10", "DP=00 M0010=41 CC=00", 1, "M0010=42 CC=00", 6 },
    { "NEG extended", 0, "70 20 00", "M2000=01 CC=00", 1, "M2000=FF CC=09", 7 },
    { "CLR indexed", 0, "6F 84", "X=2000 M2000=55 CC=00", 1, "M2000=00 CC=04", 6 },
    { "TST extended", 0, "7D 20 00", "M2000=80 CC=02", 1, "CC=08", 7 },
    { "JMP extended", 0, "7E 23 45", "", 1, "PC=2345", 4 },
    /* ---- 16-bit ---- */
    { "ADDD carry", 0, "C3 00 01", "D=FFFF CC=00", 1, "D=0000 CC=05", 4 },
    { "SUBD overflow", 0, "83 00 01", "D=8000 CC=00", 1, "D=7FFF CC=02", 4 },
    { "CMPX equal", 0, "8C 12 34", "X=1234 CC=00", 1, "CC=04", 4 },
    { "LDD", 0, "CC 80 00", "CC=00", 1, "D=8000 CC=08", 3 },
    { "STD extended", 0, "FD 20 00", "D=1234 CC=00", 1, "M2000=12 34", 6 },
    { "LDX direct", 0, "9E 10", "DP=20 M2010=AB CD", 1, "X=ABCD CC=08", 5 },
    { "LDU immediate", 0, "CE 00 00", "CC=00", 1, "U=0000 CC=04", 3 },
    { "CMPD (page 2)", 0, "10 83 10 00", "D=1000 CC=00", 1, "CC=04", 5 },
    { "LDY immediate", 0, "10 8E 12 34", "", 1, "Y=1234", 4 },
    { "STY direct", 0, "10 9F 10", "Y=BEEF DP=20", 1, "M2010=BE EF", 6 },
    { "LDS immediate", 0, "10 CE 20 00", "", 1, "S=2000", 4 },
    { "CMPU (page 3)", 0, "11 83 00 10", "U=0011 CC=00", 1, "CC=00", 5 },
    { "CMPS less", 0, "11 8C 00 10", "S=000F CC=00", 1, "CC=09", 5 },
    /* ---- indexed modes ---- */
    { "LDA 5-bit -1,X", 0, "A6 1F", "X=2001 M2000=77", 1, "A=77", 5 },
    { "LDA ,X+", 0, "A6 80", "X=2000 M2000=11", 1, "A=11 X=2001", 6 },
    { "LDD ,X++", 0, "EC 81", "X=2000 M2000=12 34", 1, "D=1234 X=2002", 8 },
    { "LDA ,-Y", 0, "A6 A2", "Y=2001 M2000=22", 1, "A=22 Y=2000", 6 },
    { "LDD ,--U", 0, "EC C3", "U=2002 M2000=56 78", 1, "D=5678 U=2000", 8 },
    { "LDA ,S", 0, "A6 E4", "S=2000 M2000=33", 1, "A=33", 4 },
    { "LDA B,X (negative B)", 0, "A6 85", "X=2010 B=F0 M2000=44", 1, "A=44", 5 },
    { "LDA A,X", 0, "A6 86", "X=2000 A=10 M2010=45", 1, "A=45", 5 },
    { "LDA n8,X", 0, "A6 88 80", "X=2080 M2000=46", 1, "A=46", 5 },
    { "LDA n16,X", 0, "A6 89 10 00", "X=1000 M2000=47", 1, "A=47", 8 },
    { "LDA D,X", 0, "A6 8B", "X=1000 D=1000 M2000=48", 1, "A=48 B=00", 8 },
    { "LDA n8,PCR", 0, "A6 8C 10", "M1013=49", 1, "A=49", 5 },
    { "LDA n16,PCR", 0, "A6 8D 10 00", "M2004=4A", 1, "A=4A", 9 },
    { "LDA [,X]", 0, "A6 94", "X=2000 M2000=30 00 M3000=4B", 1, "A=4B", 7 },
    { "LDA [n16]", 0, "A6 9F 20 00", "M2000=30 00 M3000=4C", 1, "A=4C", 9 },
    { "LDA [,X++]", 0, "A6 91", "X=2000 M2000=30 00 M3000=4D", 1, "A=4D X=2002", 10 },
    { "LEAX -2,X sets Z", 0, "30 1E", "X=0002 CC=00", 1, "X=0000 CC=04", 5 },
    { "LEAX clears Z", 0, "30 01", "X=0000 CC=04", 1, "X=0001 CC=00", 5 },
    { "LEAS leaves CC", 0, "32 7F", "S=0001 CC=00", 1, "S=0000 CC=00", 5 },
    /* ---- branches ---- */
    { "BRA back", 0, "20 FE", "", 1, "PC=1000", 3 },
    { "BEQ taken", 0, "27 10", "CC=04", 1, "PC=1012", 3 },
    { "BEQ not taken", 0, "27 10", "CC=00", 1, "PC=1002", 3 },
    { "BHI (C clear, Z clear)", 0, "22 10", "CC=00", 1, "PC=1012", 3 },
    { "BLS on carry", 0, "23 10", "CC=01", 1, "PC=1012", 3 },
    { "BGE N==V", 0, "2C 10", "CC=0A", 1, "PC=1012", 3 },
    { "BLT N!=V", 0, "2D 10", "CC=08", 1, "PC=1012", 3 },
    { "BGT not on Z", 0, "2E 10", "CC=04", 1, "PC=1002", 3 },
    { "BLE on Z", 0, "2F 10", "CC=04", 1, "PC=1012", 3 },
    { "LBRA", 0, "16 10 00", "", 1, "PC=2003", 5 },
    { "LBNE taken", 0, "10 26 10 00", "CC=00", 1, "PC=2004", 6 },
    { "LBNE not taken", 0, "10 26 10 00", "CC=04", 1, "PC=1004", 5 },
    { "BSR", 0, "8D 10", "S=3000", 1, "PC=1012 S=2FFE M2FFE=10 02", 7 },
    { "LBSR", 0, "17 10 00", "S=3000", 1, "PC=2003 S=2FFE M2FFE=10 03", 9 },
    { "JSR indexed", 0, "AD 84", "X=2000 S=3000", 1, "PC=2000 S=2FFE M2FFE=10 02", 7 },
    { "RTS", 0, "39", "S=2FFE M2FFE=12 34", 1, "PC=1234 S=3000", 5 },
    /* ---- registers ---- */
    { "TFR X,Y", 0, "1F 12", "X=1234", 1, "Y=1234", 6 },
    { "TFR A,B", 0, "1F 89", "A=55", 1, "B=55", 6 },
    { "TFR A,X (6809: $FF high)", 0, "1F 81", "A=12", 1, "X=FF12", 6 },
    { "TFR X,A (low byte)", 0, "1F 18", "X=1234", 1, "A=34", 6 },
    { "TFR CC,X (6809: CC:CC)", 0, "1F A1", "CC=05", 1, "X=0505", 6 },
    { "EXG A,B", 0, "1E 89", "A=12 B=34", 1, "A=34 B=12", 8 },
    { "EXG X,U", 0, "1E 13", "X=1111 U=2222", 1, "X=2222 U=1111", 8 },
    { "PSHS all", 0, "34 FF", "S=3000 A=01 B=02 X=0304 Y=0506 U=0708 DP=09 CC=0A", 1,
      "S=2FF4 M2FF4=0A 01 02 09 03 04 05 06 07 08 10 02", 17 },
    { "PULS all", 0, "35 FF", "S=2FF4 M2FF4=0A 01 02 09 03 04 05 06 07 08 20 00", 1,
      "S=3000 CC=0A A=01 B=02 DP=09 X=0304 Y=0506 U=0708 PC=2000", 17 },
    { "PSHU S,B", 0, "36 44", "U=3000 S=1234 B=99", 1, "U=2FFD M2FFD=99 12 34", 8 },
    { "ORCC", 0, "1A 50", "CC=00", 1, "CC=50", 3 },
    { "ANDCC", 0, "1C AF", "CC=FF", 1, "CC=AF", 3 },
    /* ---- interrupts ---- */
    { "SWI stacks everything", 0, "3F", "S=3000 A=01 B=02 CC=00 MFFFA=40 00", 1,
      "PC=4000 S=2FF4 M2FF4=80 01 02 CC=D0", 19 },
    { "SWI2 leaves the masks", 0, "10 3F", "S=3000 CC=00 MFFF4=40 00", 1, "PC=4000 CC=80", 20 },
    { "IRQ", 0, "12", "S=3000 CC=00 MFFF8=50 00 IRQ", 1, "PC=5000 S=2FF4 CC=90 M2FF4=80", 19 },
    { "IRQ masked", 0, "12", "S=3000 CC=10 MFFF8=50 00 IRQ", 1, "PC=1001", 2 },
    { "FIRQ stacks PC and CC", 0, "12", "S=3000 CC=00 MFFF6=60 00 FIRQ", 1, "PC=6000 S=2FFD CC=50 M2FFD=00 10 00", 10 },
    { "NMI needs S loaded", 0, "12 12", "CC=00 MFFFC=70 00 NMI", 1, "PC=1001", 2 },
    { "NMI after LDS", 0, "10 CE 30 00 12", "CC=50 MFFFC=70 00", 1, "S=3000", 4 },
    { "RTI full frame", 0, "3B", "S=2FF4 M2FF4=80 01 02 09 03 04 05 06 07 08 20 00", 1,
      "S=3000 CC=80 A=01 B=02 DP=09 X=0304 Y=0506 U=0708 PC=2000", 15 },
    { "RTI FIRQ frame", 0, "3B", "S=2FFD M2FFD=01 20 00", 1, "S=3000 CC=01 PC=2000", 6 },
    { "CWAI then IRQ", 0, "3C EF", "S=3000 CC=10 MFFF8=50 00 IRQ", 2, "PC=5000 S=2FF4 CC=90", 27 },
    /* ---- 6809 undocumented ---- */
    { "6809 $10 prefix on a page 0 opcode", 0, "10 86 42", "", 1, "A=42 PC=1003", 3 },

    /* ---- 6309, emulation mode ---- */
    { "6309 TFR A,X (A:A)", 1, "1F 81", "A=12", 1, "X=1212", 6 },
    { "6309 TFR X,A (high byte)", 1, "1F 18", "X=1234", 1, "A=12", 6 },
    { "6309 LDW", 1, "10 86 12 34", "CC=00", 1, "W=1234 CC=00", 4 },
    { "6309 LDQ immediate", 1, "CD 12 34 56 78", "CC=00", 1, "D=1234 W=5678 CC=00", 5 },
    { "6309 STQ extended", 1, "10 FD 20 00", "D=1234 W=5678", 1, "M2000=12 34 56 78", 9 },
    { "6309 ADDR A,B", 1, "10 30 89", "A=10 B=20 CC=00", 1, "B=30 A=10", 4 },
    { "6309 ADDR X,Y", 1, "10 30 12", "X=1000 Y=2000 CC=00", 1, "Y=3000", 4 },
    { "6309 SUBR B,A (8-bit)", 1, "10 32 98", "A=10 B=11 CC=00", 1, "A=FF CC=09", 4 },
    { "6309 CMPR D,X", 1, "10 37 01", "D=1234 X=1234 CC=00", 1, "CC=04", 4 },
    { "6309 SEXW", 1, "14", "W=8000", 1, "D=FFFF", 4 },
    { "6309 MULD", 1, "11 8F FF FE", "D=0003 CC=00", 1, "D=FFFF W=FFFA CC=08", 28 },
    { "6309 DIVD", 1, "11 8D 07", "D=0016 CC=00", 1, "A=01 B=03 CC=01", -1 },
    { "6309 DIVD negative", 1, "11 8D 07", "D=FFEA CC=00", 1, "A=FF B=FD CC=09", -1 },
    { "6309 DIVQ", 1, "11 8E 00 10", "D=0000 W=0123 CC=00", 1, "W=0012 D=0003 CC=00", -1 },
    { "6309 divide by zero traps", 1, "11 8D 00", "D=1234 S=3000 CC=00 MFFF0=55 00", 1, "PC=5500 MD=80", -1 },
    { "6309 illegal opcode traps", 1, "15", "S=3000 CC=00 MFFF0=55 00", 1, "PC=5500 MD=40 S=2FF4", -1 },
    { "6309 TFM X+,Y+", 1, "11 38 12", "W=0003 X=2000 Y=3000 M2000=11 22 33", 1, "W=0000 X=2003 Y=3003 M3000=11 22 33", 15 },
    { "6309 TFM X,Y+ (fill)", 1, "11 3B 12", "W=0002 X=2000 Y=3000 M2000=AA", 1, "X=2000 Y=3002 M3000=AA AA", 12 },
    { "6309 OIM direct", 1, "01 0F 10", "DP=20 M2010=F0 CC=02", 1, "M2010=FF CC=08", 6 },
    { "6309 AIM indexed", 1, "62 0F 84", "X=2000 M2000=F3", 1, "M2000=03", 7 },
    { "6309 TIM extended", 1, "7B 80 20 00", "M2000=80 CC=00", 1, "CC=08 M2000=80", 7 },
    { "6309 BAND A.0 with mem.7", 1, "11 30 78 10", "A=01 DP=20 M2010=00", 1, "A=00", 7 },
    { "6309 LDBT CC.0 from mem.1", 1, "11 36 08 10", "CC=00 DP=20 M2010=02", 1, "CC=01", 7 },
    { "6309 STBT B.7 into mem.0", 1, "11 37 B8 10", "B=80 DP=20 M2010=00", 1, "M2010=01", 8 },
    { "6309 CLRW", 1, "10 5F", "W=1234 CC=09", 1, "W=0000 CC=04", 3 },
    { "6309 INCE", 1, "11 4C", "E=7F CC=00", 1, "E=80 CC=0A", 3 },
    { "6309 ADDF", 1, "11 CB 01", "F=FF CC=00", 1, "F=00 CC=25", 3 },
    { "6309 PSHSW", 1, "10 38", "W=1234 S=3000", 1, "S=2FFE M2FFE=12 34", 6 },
    { "6309 LDA E,X", 1, "A6 87", "X=2000 E=10 M2010=5A", 1, "A=5A", 5 },
    { "6309 LDA ,W", 1, "A6 8F", "W=2000 M2000=5B", 1, "A=5B", 4 },
    { "6309 LDA ,W++", 1, "A6 CF", "W=2000 M2000=5C", 1, "A=5C W=2002", 7 },
    { "6309 LDMD native", 1, "11 3D 01", "MD=00", 1, "MD=01", 5 },

    /* ---- 6309 native mode ---- */
    { "native LDA direct 3 cycles", 2, "96 10", "DP=20 M2010=42", 1, "A=42", 3 },
    { "native NOP 1 cycle", 2, "12", "", 1, "PC=1001", 1 },
    { "native SWI stacks W", 2, "3F", "S=3000 A=01 B=02 E=03 F=04 CC=00 MFFFA=40 00", 1,
      "PC=4000 S=2FF2 M2FF2=80 01 02 03 04", 21 },
    { "native RTI pulls W", 2, "3B", "S=2FF2 M2FF2=80 01 02 03 04 09 03 04 05 06 07 08 20 00", 1,
      "S=3000 A=01 B=02 E=03 F=04 DP=09 X=0304 PC=2000", 17 },
    { "native FIRQ with FM stacks all", 2, "12", "S=3000 CC=00 MD=03 MFFF6=60 00 FIRQ", 1,
      "PC=6000 S=2FF2 CC=D0", 21 },
};

/* ---- The machine: flat 64 KB ---- */

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

static int parse_hex_bytes(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') {
            s++;
        }
        /* two hex digits, then a space or the end ("CC=" is a register) */
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]) || (s[2] && s[2] != ' ')) {
            break;
        }
        char b[3] = { s[0], s[1], 0 };
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
        s += 2;
    }
    return n;
}

/* Apply (set) or compare (check) a register/memory text; failures to buf */
static bool apply(m6809_t *c, const char *text, bool check, char *why, size_t len)
{
    char tok[64];
    const char *p = text;
    bool ok = true;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        int n = 0;
        while (p[n] && p[n] != ' ' && n < 63) {
            tok[n] = p[n];
            n++;
        }
        tok[n] = 0;
        p += n;
        if (!strcmp(tok, "IRQ")) { c->irq = 1; continue; }
        if (!strcmp(tok, "FIRQ")) { c->firq = 1; continue; }
        if (!strcmp(tok, "NMI")) { m6809_nmi(c); continue; }
        char *eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        const char *name = tok, *val = eq + 1;
        if (name[0] == 'M' && strlen(name) == 5 && isxdigit((unsigned char)name[1])) {
            /* memory: this token and the following hex bytes */
            uint16_t addr = (uint16_t)strtoul(name + 1, NULL, 16);
            uint8_t bytes[64];
            char buf[256];
            snprintf(buf, sizeof(buf), "%s%s", val, p);
            int count = parse_hex_bytes(buf, bytes, 64);
            /* skip the extra bytes in the text */
            const char *q = p;
            for (int i = 1; i < count; i++) {
                while (*q == ' ') q++;
                q += 2;
            }
            p = q;
            for (int i = 0; i < count; i++) {
                uint16_t a = (uint16_t)(addr + i);
                if (check) {
                    if (s_mem[a] != bytes[i]) {
                        ok = false;
                        size_t at = strlen(why);
                        snprintf(why + at, len - at, " M%04X=%02X(want %02X)", a, s_mem[a], bytes[i]);
                    }
                } else {
                    s_mem[a] = bytes[i];
                }
            }
            continue;
        }
        unsigned v = (unsigned)strtoul(val, NULL, 16);
        unsigned *dummy = NULL;
        (void)dummy;
        unsigned have = 0;
#define REG8(N, F)  if (!strcmp(name, N)) { if (check) have = c->F; else c->F = (uint8_t)v; goto done; }
#define REG16(N, F) if (!strcmp(name, N)) { if (check) have = c->F; else c->F = (uint16_t)v; goto done; }
        REG8("A", a) REG8("B", b) REG8("E", e) REG8("F", f) REG8("DP", dp) REG8("CC", cc) REG8("MD", md)
        REG16("D", d) REG16("W", w) REG16("X", x) REG16("Y", y) REG16("U", u) REG16("S", s) REG16("PC", pc) REG16("V", v)
        fprintf(stderr, "unknown register %s\n", name);
        exit(2);
    done:
        if (!strcmp(name, "S") && !check) {
            c->nmi_armed = true;
        }
        if (check && have != v) {
            ok = false;
            size_t at = strlen(why);
            snprintf(why + at, len - at, " %s=%X(want %X)", name, have, v);
        }
    }
    return ok;
}

int main(int argc, char **argv)
{
    bool verbose = argc > 1 && !strcmp(argv[1], "-v");
    int failed = 0, n = (int)(sizeof(cases) / sizeof(cases[0]));
    m6809_bus_t bus = { bus_read, bus_write, NULL };
    for (int i = 0; i < n; i++) {
        const test_case_t *t = &cases[i];
        m6809_t cpu, *c = &cpu;
        memset(s_mem, 0, sizeof(s_mem));
        m6809_init(c, &bus, t->cpu != 0);
        /* no pages: everything through the bus (the page path is the same code) */
        c->pc = 0x1000;
        c->s = 0x8000;
        c->nmi_armed = false;
        uint8_t code[32];
        int len = parse_hex_bytes(t->code, code, 32);
        char why[512] = "";
        apply(c, t->before, false, why, sizeof(why));
        if (t->cpu == 2) {
            c->md |= M6309_NM;
        }
        memcpy(s_mem + c->pc, code, (size_t)len);
        /* "PC=" in before: code there; S= arms NMIs only through the program */
        if (strstr(t->before, "S=") == NULL) {
            c->nmi_armed = false;
        }
        uint64_t start = c->cycles;
        for (int s = 0; s < t->steps; s++) {
            c->until = c->cycles + 1000;
            m6809_step(c);
        }
        int cycles = (int)(c->cycles - start);
        bool ok = apply(c, t->after, true, why, sizeof(why));
        if (t->cycles >= 0 && cycles != t->cycles) {
            ok = false;
            size_t at = strlen(why);
            snprintf(why + at, sizeof(why) - at, " cycles %d(want %d)", cycles, t->cycles);
        }
        if (!ok) {
            failed++;
        }
        if (!ok || verbose) {
            printf("%s %s:%s\n", ok ? "ok  " : "FAIL", t->name, why);
        }
    }
    printf("%d cases, %d failed\n", n, failed);
    return failed ? 1 : 0;
}
