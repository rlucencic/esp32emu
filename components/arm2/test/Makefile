# Host tests of the ARM2 core: make -C components/arm2/test test
#   build/cases [-v]      instruction cases (R15 and the PSR, modes, exceptions,
#                         transfers, aborts, the shifter, cycles, disassembly)
#   build/bench           how fast the core runs on this machine
#   diff.py [N]           N random instructions against Unicorn's ARM (needs
#                         `pip install unicorn`): data processing, multiplies,
#                         loads and stores, block transfers
BUILD   := build
CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -I../include -I../../emudbg/include -DEMUDBG_OFF
SRCS    := ../arm2.c ../arm2_dis.c

all: $(BUILD)/cases $(BUILD)/libarm2.so $(BUILD)/bench

$(BUILD)/cases: cases.c $(SRCS) ../include/arm2.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< $(SRCS) -o $@

$(BUILD)/bench: bench.c $(SRCS) ../include/arm2.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< $(SRCS) -o $@

$(BUILD)/libarm2.so: diff_lib.c $(SRCS) ../include/arm2.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -shared -fPIC $< $(SRCS) -o $@

test: all
	$(BUILD)/cases

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
