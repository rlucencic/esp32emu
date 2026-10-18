/*
 * 65C816 disassembler (debugging), apart from the core so that its tables
 * stay in flash (linker.lf puts the core's read-only data in internal RAM).
 * WDC syntax: "LDA [$12],Y", "MVN $src,$dst", branch targets as addresses.
 */
#include <stdio.h>
#include "w65816.h"

enum {
    IMP, ACC, IMM_M, IMM_X, IMM8, SIG, DP, DPX, DPY, DPI, DPIX, DPIY, DPL, DPLY,
    ABS, ABSX, ABSY, ABSI, ABSIX, ABSL, LONG, LONGX, SR, SRIY, REL, RELL, MOVE,
};

static const char s_names[256][4] = {
    "BRK","ORA","COP","ORA","TSB","ORA","ASL","ORA","PHP","ORA","ASL","PHD","TSB","ORA","ASL","ORA",
    "BPL","ORA","ORA","ORA","TRB","ORA","ASL","ORA","CLC","ORA","INC","TCS","TRB","ORA","ASL","ORA",
    "JSR","AND","JSL","AND","BIT","AND","ROL","AND","PLP","AND","ROL","PLD","BIT","AND","ROL","AND",
    "BMI","AND","AND","AND","BIT","AND","ROL","AND","SEC","AND","DEC","TSC","BIT","AND","ROL","AND",
    "RTI","EOR","WDM","EOR","MVP","EOR","LSR","EOR","PHA","EOR","LSR","PHK","JMP","EOR","LSR","EOR",
    "BVC","EOR","EOR","EOR","MVN","EOR","LSR","EOR","CLI","EOR","PHY","TCD","JML","EOR","LSR","EOR",
    "RTS","ADC","PER","ADC","STZ","ADC","ROR","ADC","PLA","ADC","ROR","RTL","JMP","ADC","ROR","ADC",
    "BVS","ADC","ADC","ADC","STZ","ADC","ROR","ADC","SEI","ADC","PLY","TDC","JMP","ADC","ROR","ADC",
    "BRA","STA","BRL","STA","STY","STA","STX","STA","DEY","BIT","TXA","PHB","STY","STA","STX","STA",
    "BCC","STA","STA","STA","STY","STA","STX","STA","TYA","STA","TXS","TXY","STZ","STA","STZ","STA",
    "LDY","LDA","LDX","LDA","LDY","LDA","LDX","LDA","TAY","LDA","TAX","PLB","LDY","LDA","LDX","LDA",
    "BCS","LDA","LDA","LDA","LDY","LDA","LDX","LDA","CLV","LDA","TSX","TYX","LDY","LDA","LDX","LDA",
    "CPY","CMP","REP","CMP","CPY","CMP","DEC","CMP","INY","CMP","DEX","WAI","CPY","CMP","DEC","CMP",
    "BNE","CMP","CMP","CMP","PEI","CMP","DEC","CMP","CLD","CMP","PHX","STP","JML","CMP","DEC","CMP",
    "CPX","SBC","SEP","SBC","CPX","SBC","INC","SBC","INX","SBC","NOP","XBA","CPX","SBC","INC","SBC",
    "BEQ","SBC","SBC","SBC","PEA","SBC","INC","SBC","SED","SBC","PLX","XCE","JSR","SBC","INC","SBC",
};

static const uint8_t s_modes[256] = {
    SIG,  DPIX, SIG,  SR,   DP,   DP,   DP,   DPL,  IMP,  IMM_M,ACC,  IMP,  ABS,  ABS,  ABS,  LONG,
    REL,  DPIY, DPI,  SRIY, DP,   DPX,  DPX,  DPLY, IMP,  ABSY, ACC,  IMP,  ABS,  ABSX, ABSX, LONGX,
    ABS,  DPIX, LONG, SR,   DP,   DP,   DP,   DPL,  IMP,  IMM_M,ACC,  IMP,  ABS,  ABS,  ABS,  LONG,
    REL,  DPIY, DPI,  SRIY, DPX,  DPX,  DPX,  DPLY, IMP,  ABSY, ACC,  IMP,  ABSX, ABSX, ABSX, LONGX,
    IMP,  DPIX, SIG,  SR,   MOVE, DP,   DP,   DPL,  IMP,  IMM_M,ACC,  IMP,  ABS,  ABS,  ABS,  LONG,
    REL,  DPIY, DPI,  SRIY, MOVE, DPX,  DPX,  DPLY, IMP,  ABSY, IMP,  IMP,  LONG, ABSX, ABSX, LONGX,
    IMP,  DPIX, RELL, SR,   DP,   DP,   DP,   DPL,  IMP,  IMM_M,ACC,  IMP,  ABSI, ABS,  ABS,  LONG,
    REL,  DPIY, DPI,  SRIY, DPX,  DPX,  DPX,  DPLY, IMP,  ABSY, IMP,  IMP,  ABSIX,ABSX, ABSX, LONGX,
    REL,  DPIX, RELL, SR,   DP,   DP,   DP,   DPL,  IMP,  IMM_M,IMP,  IMP,  ABS,  ABS,  ABS,  LONG,
    REL,  DPIY, DPI,  SRIY, DPX,  DPX,  DPY,  DPLY, IMP,  ABSY, IMP,  IMP,  ABS,  ABSX, ABSX, LONGX,
    IMM_X,DPIX, IMM_X,SR,   DP,   DP,   DP,   DPL,  IMP,  IMM_M,IMP,  IMP,  ABS,  ABS,  ABS,  LONG,
    REL,  DPIY, DPI,  SRIY, DPX,  DPX,  DPY,  DPLY, IMP,  ABSY, IMP,  IMP,  ABSX, ABSX, ABSY, LONGX,
    IMM_X,DPIX, IMM8, SR,   DP,   DP,   DP,   DPL,  IMP,  IMM_M,IMP,  IMP,  ABS,  ABS,  ABS,  LONG,
    REL,  DPIY, DPI,  SRIY, DPI,  DPX,  DPX,  DPLY, IMP,  ABSY, IMP,  IMP,  ABSL, ABSX, ABSX, LONGX,
    IMM_X,DPIX, IMM8, SR,   DP,   DP,   DP,   DPL,  IMP,  IMM_M,IMP,  IMP,  ABS,  ABS,  ABS,  LONG,
    REL,  DPIY, DPI,  SRIY, ABS,  DPX,  DPX,  DPLY, IMP,  ABSY, IMP,  IMP,  ABSIX,ABSX, ABSX, LONGX,
};

int w65816_disassemble(const w65816_t *cpu, uint32_t addr, char *buf, int len)
{
    /* The operand bytes follow in the program bank (the PC wraps there) */
    uint32_t bank = addr & 0xFF0000;
    uint8_t op = w65816_peek(cpu, addr);
    uint8_t b1 = w65816_peek(cpu, bank | ((addr + 1) & 0xFFFF));
    uint8_t b2 = w65816_peek(cpu, bank | ((addr + 2) & 0xFFFF));
    uint8_t b3 = w65816_peek(cpu, bank | ((addr + 3) & 0xFFFF));
    unsigned w = b1 | b2 << 8, l = w | (unsigned)b3 << 16;
    const char *n = s_names[op];
    int mode = s_modes[op];
    bool m16 = !cpu->e && !(cpu->p & W65816_M), x16 = !cpu->e && !(cpu->p & W65816_X);
    switch (mode) {
    case IMP: snprintf(buf, len, "%s", n); return 1;
    case ACC: snprintf(buf, len, "%s A", n); return 1;
    case IMM_M:
    case IMM_X:
        if (mode == IMM_M ? m16 : x16) {
            snprintf(buf, len, "%s #$%04X", n, w);
            return 3;
        }
        /* fall through */
    case IMM8: snprintf(buf, len, "%s #$%02X", n, b1); return 2;
    case SIG: snprintf(buf, len, "%s $%02X", n, b1); return 2;
    case DP: snprintf(buf, len, "%s $%02X", n, b1); return 2;
    case DPX: snprintf(buf, len, "%s $%02X,X", n, b1); return 2;
    case DPY: snprintf(buf, len, "%s $%02X,Y", n, b1); return 2;
    case DPI: snprintf(buf, len, "%s ($%02X)", n, b1); return 2;
    case DPIX: snprintf(buf, len, "%s ($%02X,X)", n, b1); return 2;
    case DPIY: snprintf(buf, len, "%s ($%02X),Y", n, b1); return 2;
    case DPL: snprintf(buf, len, "%s [$%02X]", n, b1); return 2;
    case DPLY: snprintf(buf, len, "%s [$%02X],Y", n, b1); return 2;
    case SR: snprintf(buf, len, "%s $%02X,S", n, b1); return 2;
    case SRIY: snprintf(buf, len, "%s ($%02X,S),Y", n, b1); return 2;
    case REL: snprintf(buf, len, "%s $%04X", n, (unsigned)(uint16_t)(addr + 2 + (int8_t)b1)); return 2;
    case RELL: snprintf(buf, len, "%s $%04X", n, (unsigned)(uint16_t)(addr + 3 + w)); return 3;
    case ABS: snprintf(buf, len, "%s $%04X", n, w); return 3;
    case ABSX: snprintf(buf, len, "%s $%04X,X", n, w); return 3;
    case ABSY: snprintf(buf, len, "%s $%04X,Y", n, w); return 3;
    case ABSI: snprintf(buf, len, "%s ($%04X)", n, w); return 3;
    case ABSIX: snprintf(buf, len, "%s ($%04X,X)", n, w); return 3;
    case ABSL: snprintf(buf, len, "%s [$%04X]", n, w); return 3;
    case MOVE: snprintf(buf, len, "%s $%02X,$%02X", n, b2, b1); return 3;   /* source, destination */
    case LONG: snprintf(buf, len, "%s $%06X", n, l); return 4;
    default: snprintf(buf, len, "%s $%06X,X", n, l); return 4;
    }
}
