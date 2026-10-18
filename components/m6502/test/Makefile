# Host tests of the 6502 core: make -C components/m6502/test
#   build/functional <6502_functional_test.bin>   (Klaus Dormann's test)
#   build/opcodes                                 (every opcode once: cycles, JAMs)
BUILD   := build
CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -I../include -I../../emudbg/include -DEMUDBG_OFF

all: $(BUILD)/functional $(BUILD)/opcodes

$(BUILD)/%: %.c ../m6502.c ../include/m6502.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< ../m6502.c -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
