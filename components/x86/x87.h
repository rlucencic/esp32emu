/*
 * The coprocessor (x87.c), for x86.c
 */
#pragma once

#include "x86.h"

/* After power-on (all of it) and FINIT */
void x87_reset(x87_t *f, bool i8087);
void x87_init(x87_t *f);

/* A register's value as a double (the debugger: rounded, the range clipped) */
double x87_to_double(x87_reg_t r);

/* An ESC instruction's memory operand (op D8-DF, the ModRM byte's reg):
 * bytes read before it runs, bytes it writes
 */
void x87_operand(uint8_t op, uint8_t reg, int *read, int *write);

/* One ESC instruction: mem holds the operand's bytes (read, and those it
 * writes as they were), dp_off and dp_seg where (its selector); its clocks
 */
int x87_exec(x86_t *c, uint8_t op, uint8_t mod, uint8_t reg, uint8_t rm, uint8_t *mem, uint16_t dp_off,
             uint16_t dp_seg);
