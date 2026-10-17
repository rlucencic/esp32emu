# Host test of the Z80 core: make -C components/z80/test
#   build/z80_test     <prelim.com | zexdoc.cim | zexall.cim>   (T-states)
#   build/z80_test_cpc <...>                                     (CPC microseconds)
BUILD   := build
CFLAGS  := -O2 -g -Wall -Wextra -I../include

all: $(BUILD)/z80_test $(BUILD)/z80_test_cpc

$(BUILD)/z80_test: z80_test.c ../z80.c ../include/z80.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DCONFIG_Z80_CONTENTION=1 z80_test.c ../z80.c -o $@

$(BUILD)/z80_test_cpc: z80_test.c ../z80.c ../include/z80.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DCONFIG_Z80_TIMING_CPC=1 z80_test.c ../z80.c -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
