# Host tests of pro1x-fmd and the GENI UART's arithmetic
# (freebsd/fm/README.md), under ASan and UBSan:
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

$(OUT)/test_geni: test_geni.c ../sys/dev/uart/uart_qcom_geni_calc.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_geni.c

$(OUT)/test_q6: test_q6.c ../../dsp/tools/pro1x-q6/pro1x-q6.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_q6.c

FMA = ../tools/pro1x-fmaudio
$(OUT)/test_fmaudio: test_fmaudio.c $(FMA)/pro1x-fmaudio.c $(FMA)/ngd.c $(FMA)/afe.c $(FMA)/mmio.c $(FMA)/fmaudio.h ../../dsp/tools/pro1x-q6/pro1x-q6.c | $(OUT)
	$(CC) $(CFLAGS) -I$(FMA) -o $@ test_fmaudio.c $(FMA)/ngd.c $(FMA)/afe.c $(FMA)/mmio.c

$(OUT)/fakechip: fakechip.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ fakechip.c

# the app's FreeBSD backend, built on the PC (GTK 2, a stub of libhildon)
$(OUT)/test_app: test_app.c ../../../apps/pro1x-fm/pro1x-fm.c hildon-stub/hildon/hildon.h | $(OUT)
	$(CC) -O1 -g -Wall -Wno-deprecated-declarations -U__linux__ \
	    -DLOCALEDIR='"/usr/share/locale"' -DFMD_SOCKET='"$(OUT)/fm.sock"' \
	    -Ihildon-stub $$(pkg-config --cflags gtk+-2.0 gthread-2.0) -o $@ \
	    test_app.c $$(pkg-config --libs gtk+-2.0 gthread-2.0)

firmware: | $(OUT)
	for f in cmbtfw12.tlv cmnv12.bin; do \
		if [ -f $(FW)/$$f ]; then cp $(FW)/$$f $(OUT)/fw/qca/; \
		else zstd -dqf $(FW)/$$f.zst -o $(OUT)/fw/qca/$$f; fi; \
	done

APP = $(shell pkg-config --exists gtk+-2.0 && echo $(OUT)/test_app)

test: $(APP) $(OUT)/test_qcabt $(OUT)/test_geni $(OUT)/test_q6 $(OUT)/test_fmaudio $(OUT)/pro1x-fmd $(OUT)/pro1x-fmctl $(OUT)/fakechip firmware
	$(OUT)/test_geni
	$(OUT)/test_q6
	$(OUT)/test_fmaudio > $(OUT)/test_fmaudio.log || { cat $(OUT)/test_fmaudio.log; exit 1; }
	tail -1 $(OUT)/test_fmaudio.log
	$(OUT)/test_qcabt $(OUT)/fw/qca
	OUT=$(OUT) sh ./run.sh

clean:
	rm -rf $(OUT)

.PHONY: all test firmware clean
