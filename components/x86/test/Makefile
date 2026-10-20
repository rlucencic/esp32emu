# Host tests of the x86 core: make -C components/x86/test test
#   build/ssts [-m 8088|8086|286] [-u] [-i] [-w] [-v] files   (the SingleStepTests suites, MOO files)
# `make data` fetches the suites (8088 ~0.5 GB, 8086 0.1, 80286 0.3) into data/ (not in git).
# The 286's: -i leaves out the flags its interrupt 0 pushes after DIV, IDIV
# and AAM (undefined, from its divider: not reproduced); 17 of 1,478,000
# fail then (an IDIV quotient quirk, AAM 0's parity, an ENTER, MOV Sreg).
BUILD   := build
CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -I../include -I../../emudbg/include -DEMUDBG_OFF
SRCS    := ../x86.c ../x87.c ../x86_dis.c
SSTS    := https://raw.githubusercontent.com/SingleStepTests

all: $(BUILD)/ssts $(BUILD)/masks_8088.txt $(BUILD)/masks_8086.txt $(BUILD)/masks_80286.txt

$(BUILD)/ssts: ssts.c $(SRCS) ../include/x86.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) ssts.c $(SRCS) -lz -o $@

$(BUILD)/masks_%.txt: data/%/metadata.json masks.py
	@mkdir -p $(BUILD)
	python3 -I masks.py $< > $@

# The suites (the 8088's v2 and the 8086's with the undefined flags as the chips leave them)
test: all
	$(BUILD)/ssts -m 8088 -u data/8088/*.MOO.gz
	$(BUILD)/ssts -m 8086 -u data/8086/*.MOO.gz
	-$(BUILD)/ssts -m 286 -i data/80286/*.MOO.gz
	$(BUILD)/ssts -m 8088 -u -w data/8088/*.MOO.gz
	-$(BUILD)/ssts -m 286 -i -w data/80286/*.MOO.gz

# data/<cpu>/: the suite's MOO files and metadata (repository, folders)
fetch = mkdir -p data/$(1) && cd data/$(1) && curl -sfL -O $(SSTS)/$(2)/main/$(3)/metadata.json && \
	for op in $$(curl -sf https://api.github.com/repos/SingleStepTests/$(2)/contents/$(4) | \
	             python3 -I -c 'import json,sys; print(" ".join(e["name"] for e in json.load(sys.stdin) if e["name"].endswith(".MOO.gz")))'); do \
	    [ -s $$op ] || curl -sfL -O $(SSTS)/$(2)/main/$(4)/$$op; done

data:
	$(call fetch,8088,8088,v2,v2_binary)
	$(call fetch,8086,8086,v1,v1_binary)
	$(call fetch,80286,80286,v1_real_mode,v1_real_mode)

clean:
	rm -rf $(BUILD)

.PHONY: all test data clean
