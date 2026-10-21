# Host tests of the counters: make -C components/diskspace/test
#   each test_<x>.c is a program of its own (images made in it, or files
#   given as arguments: build/test_<x> FILE...), all the counters linked in
BUILD   := build
CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -I../include
SRCS    := $(wildcard ../*.c)
TESTS   := $(patsubst test_%.c,$(BUILD)/test_%,$(wildcard test_*.c))

run: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

$(BUILD)/test_%: test_%.c test.h $(SRCS) ../include/diskspace.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< $(SRCS) -o $@

clean:
	rm -rf $(BUILD)

.PHONY: run clean
