# Host tests of the 65C816 core: make -C components/w65816/test
#   build/ssts <v1 folder | XX.n.json ...>   (the SingleStepTests 65816 suite)
#   build/irq                                (interrupts, WAI, STP, MVN, penalty, WDM)
#   build/diff [seeds]                       (w65816_run() against w65816_step() on random code)
#   build/speed                              (instructions per second)
BUILD   := build
CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -I../include
SRCS    := ../w65816.c ../w65816_dis.c

all: $(BUILD)/ssts $(BUILD)/irq $(BUILD)/diff $(BUILD)/speed

$(BUILD)/%: %.c $(SRCS) ../include/w65816.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< $(SRCS) -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
