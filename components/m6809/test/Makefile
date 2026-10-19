# Host tests of the 6809 / 6309 core: make -C components/m6809/test
#   build/cases [-v]      instruction cases (results, flags, memory, cycles)
#   build/opcodes         every opcode once, both CPUs: lengths against the
#                         disassembler, cycles against the data sheets' tables
BUILD   := build
CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -I../include -I../../emudbg/include -DEMUDBG_OFF
SRCS    := ../m6809.c ../m6809_dis.c

all: $(BUILD)/cases $(BUILD)/opcodes

$(BUILD)/%: %.c $(SRCS) ../include/m6809.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< $(SRCS) -o $@

test: all
	$(BUILD)/cases
	$(BUILD)/opcodes

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
