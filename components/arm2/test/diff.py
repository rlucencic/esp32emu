#!/usr/bin/env python3
"""
The ARM2 core against Unicorn's ARM (an ARMv7 in ARM state), one random
instruction at a time: data processing (every opcode and operand form),
MUL/MLA, LDR/STR (immediate and register offsets, byte and word, pre and
post indexed, write back), LDM/STM, B/BL, under random conditions and
flags. Only what both architectures define alike: no R15 operands (the
ARM2 mixes the PSR into it), aligned word transfers (the ARM2 rotates
unaligned loads), no write back into a loaded or listed base, the C flag
after a multiply ignored, R14 after BL without the ARM2's PSR bits.

    make -C components/arm2/test && python3 components/arm2/test/diff.py [N] [seed]
"""
import ctypes
import os
import random
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                               UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
                               UC_ARM_REG_R12, UC_ARM_REG_R13, UC_ARM_REG_R14, UC_ARM_REG_PC,
                               UC_ARM_REG_CPSR)

MEM = 64 * 1024
CODE = 0x1000
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4,
        UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
        UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_R13, UC_ARM_REG_R14]

lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'libarm2.so'))
lib.diff_init.restype = ctypes.POINTER(ctypes.c_uint8)
U32x16 = ctypes.c_uint32 * 16


def rnd_reg(exclude=()):
    while True:
        r = random.randrange(15)
        if r not in exclude:
            return r


def data_processing(regs):
    opc = random.randrange(16)
    s = 1 if 8 <= opc <= 11 else random.randrange(2)
    rd, rn = rnd_reg(), rnd_reg()
    if 8 <= opc <= 11:
        rd = 0                  # should be zero on the ARMv7 (the ARM2's R15 here is TEQP)
    if opc in (13, 15):
        rn = 0                  # MOV, MVN: should be zero
    if random.randrange(2):
        op2 = (1 << 25) | (random.randrange(16) << 8) | random.randrange(256)
    elif random.randrange(2):
        op2 = (random.randrange(32) << 7) | (random.randrange(4) << 5) | rnd_reg()
    else:
        rs = rnd_reg()
        # shift amounts around 0, 32 and beyond, in the low byte
        regs[rs] = (random.getrandbits(24) << 8) | random.choice([0, 1, 4, 31, 32, 33, 40, 255,
                                                                   random.randrange(256)])
        op2 = (rs << 8) | (random.randrange(4) << 5) | 0x10 | rnd_reg()
    return (opc << 21) | (s << 20) | (rn << 16) | (rd << 12) | op2, 'dp'


def multiply(regs):
    rd = rnd_reg()
    rm = rnd_reg((rd,))
    rs, rn = rnd_reg(), rnd_reg()
    acc, s = random.randrange(2), random.randrange(2)
    if not acc:
        rn = 0                  # MUL: should be zero
    return (acc << 21) | (s << 20) | (rd << 16) | (rn << 12) | (rs << 8) | 0x90 | rm, 'mul' if s else 'dp'


def single_transfer(regs):
    load, byte = random.randrange(2), random.randrange(2)
    pre, up = random.randrange(2), random.randrange(2)
    wb = random.randrange(2)
    rn = rnd_reg()
    rd = rnd_reg((rn,)) if (wb or not pre) else rnd_reg()
    regs[rn] = 0x6000 + random.randrange(0x1000) * (1 if byte else 4)
    if random.randrange(2):
        off = random.randrange(0x1000) & (0xFFF if byte else 0xFFC)
        op = off
    else:
        rm = rnd_reg((rn,))
        if rm == rd and (wb or not pre):
            rm = rnd_reg((rn, rd))
        shift = random.randrange(3)
        regs[rm] = random.randrange(0x400) * (1 if byte else 4)
        op = (1 << 25) | (shift << 7) | rm          # LSL #shift
    return ((1 << 26) | (pre << 24) | (up << 23) | (byte << 22) | (wb << 21) | (load << 20) |
            (rn << 16) | (rd << 12) | op), 'mem'


def block_transfer(regs):
    load = random.randrange(2)
    pre, up, wb = random.randrange(2), random.randrange(2), random.randrange(2)
    rn = rnd_reg()
    lst = random.getrandbits(15) or 1
    if wb or load:
        lst &= ~(1 << rn)
        lst = lst or (1 << ((rn + 1) % 15))
    regs[rn] = 0x6000 + random.randrange(0x1000) * 4
    return ((4 << 25) | (pre << 24) | (up << 23) | (wb << 21) | (load << 20) | (rn << 16) | lst), 'mem'


def branch(regs):
    link = random.randrange(2)
    off = random.randrange(-0x200, 0x200)
    return (5 << 25) | (link << 24) | (off & 0xFFFFFF), 'branch'


KINDS = [data_processing] * 6 + [multiply] * 2 + [single_transfer] * 3 + [block_transfer] * 2 + [branch]


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    random.seed(seed)
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(0, MEM)
    failures = 0
    for i in range(n):
        regs = [random.getrandbits(32) if random.randrange(4) else random.choice([0, 1, 0x7FFFFFFF,
                0x80000000, 0xFFFFFFFF]) for _ in range(15)]
        op, kind = random.choice(KINDS)(regs)
        op |= random.randrange(15) << 28 if random.randrange(3) == 0 else 0xE0000000
        nzcv = random.randrange(16) << 28
        mem = bytearray(random.getrandbits(8) for _ in range(256)) * (MEM // 256)
        mem[CODE:CODE + 4] = op.to_bytes(4, 'little')

        uc.mem_write(0, bytes(mem))
        uc.reg_write(UC_ARM_REG_CPSR, 0x10 | nzcv)     # first: user mode's R13, R14
        for r, v in zip(REGS, regs):
            uc.reg_write(r, v)
        uc.reg_write(UC_ARM_REG_PC, CODE)
        try:
            uc.emu_start(CODE, CODE + 4, count=1)
        except Exception as e:
            failures += 1
            print('&%08X: Unicorn: %s' % (op, e))
            continue
        u_regs = [uc.reg_read(r) for r in REGS]
        u_pc = uc.reg_read(UC_ARM_REG_PC)
        u_nzcv = uc.reg_read(UC_ARM_REG_CPSR) & 0xF0000000
        u_mem = uc.mem_read(0, MEM)

        m = lib.diff_init()
        ctypes.memmove(m, bytes(mem), MEM)
        lib.diff_set(U32x16(*regs, CODE), nzcv)
        vector = lib.diff_step()
        a = U32x16()
        a_nzcv = ctypes.c_uint32()
        lib.diff_get(a, ctypes.byref(a_nzcv))
        a_regs = list(a)[:15]
        a_mem = ctypes.string_at(m, MEM)

        if kind == 'branch':
            a_regs[14] &= 0x03FFFFFC
            u_regs[14] &= 0x03FFFFFC
        flag_mask = 0xD0000000 if kind == 'mul' else 0xF0000000
        bad = []
        if vector >= 0:
            bad.append('exception at &%X' % vector)
        if a_regs != u_regs:
            bad += ['R%d &%08X (Unicorn &%08X)' % (r, a_regs[r], u_regs[r]) for r in range(15)
                    if a_regs[r] != u_regs[r]]
        if (a_nzcv.value ^ u_nzcv) & flag_mask:
            bad.append('NZCV %X (Unicorn %X)' % (a_nzcv.value >> 28, u_nzcv >> 28))
        if a[15] != u_pc:
            bad.append('PC &%X (Unicorn &%X)' % (a[15], u_pc))
        if a_mem != bytes(u_mem):
            at = next(j for j in range(MEM) if a_mem[j] != u_mem[j])
            bad.append('memory at &%X' % at)
        if bad:
            failures += 1
            if failures <= 20:
                print('&%08X (NZCV %X): %s' % (op, nzcv >> 28, '; '.join(bad)))
                print('    in: ' + ' '.join('R%d=%08X' % (r, regs[r]) for r in range(15)))
    print('%d of %d instructions agree' % (n - failures, n))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
