# Host tests of pro1x-fmd (freebsd/fm/README.md), under ASan and UBSan:
#   make -C freebsd/fm/test          the unit tests and the run against
#                                    a simulated WCN3950 on a pty
# The firmware: linux-firmware's qca/cmbtfw12.tlv and qca/cmnv12.bin
# (FW=, default the PC's /usr/lib/firmware/qca, .zst unpacked).
CC ?= cc
CFLAGS = -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
	-fno-omit-frame-pointer -I../tools/pro1x-fmd
D = ../tools/pro1x-fmd
FW ?= /usr/lib/firmware/qca
OUT ?= /tmp/pro1x-fm-test

all: test

$(OUT):
	mkdir -p $(OUT)/fw/qca

$(OUT)/test_qcabt: test_qcabt.c $(D)/qcabt.c $(D)/qcafm.c $(D)/qcabt.h $(D)/qcafm.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_qcabt.c $(D)/qcabt.c $(D)/qcafm.c

$(OUT)/pro1x-fmd: $(D)/pro1x-fmd.c $(D)/qcabt.c $(D)/qcafm.c $(D)/qcabt.h $(D)/qcafm.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(D)/pro1x-fmd.c $(D)/qcabt.c $(D)/qcafm.c

$(OUT)/pro1x-fmctl: ../tools/pro1x-fmctl/pro1x-fmctl.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ ../tools/pro1x-fmctl/pro1x-fmctl.c

$(OUT)/fakechip: fakechip.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ fakechip.c

firmware: | $(OUT)
	for f in cmbtfw12.tlv cmnv12.bin; do \
		if [ -f $(FW)/$$f ]; then cp $(FW)/$$f $(OUT)/fw/qca/; \
		else zstd -dqf $(FW)/$$f.zst -o $(OUT)/fw/qca/$$f; fi; \
	done

test: $(OUT)/test_qcabt $(OUT)/pro1x-fmd $(OUT)/pro1x-fmctl $(OUT)/fakechip firmware
	$(OUT)/test_qcabt $(OUT)/fw/qca
	OUT=$(OUT) sh ./run.sh

clean:
	rm -rf $(OUT)

.PHONY: all test firmware clean
