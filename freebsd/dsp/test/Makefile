# Host tests of the DSP stack's protocol code (freebsd/dsp/README.md):
#   make -C freebsd/dsp/test
SRC=	../sys/arm64/qualcomm/dsp
CFLAGS?=	-O2 -g
CFLAGS+=	-Wall -Wextra -Wno-unused-parameter -I$(SRC) -pthread -fsanitize=address,undefined

all: run

glink_test: glink_test.c $(SRC)/glink_core.c $(SRC)/glink.h $(SRC)/glink_os.h
	$(CC) $(CFLAGS) -o $@ glink_test.c $(SRC)/glink_core.c

qrtr_test: qrtr_test.c $(SRC)/qrtr_core.c $(SRC)/qrtr.h $(SRC)/glink_os.h
	$(CC) $(CFLAGS) -o $@ qrtr_test.c $(SRC)/qrtr_core.c

# ath10k's QMI message tables (FreeBSD's source tree as build.sh unpacks it)
ATH10K?=	../../../../freebsd/src/sys/contrib/dev/athk/ath10k
QMI_SRCS=	qmi_test.c $(SRC)/qmi_core.c $(SRC)/qmi_encdec.c $(SRC)/qrtr_core.c \
		$(ATH10K)/qmi_wlfw_v01.c

# Linux's encoder does arithmetic on void pointers, as in the kernel build
qmi_test: $(QMI_SRCS) $(SRC)/qcom_qmi.h $(SRC)/qrtr.h $(SRC)/glink_os.h
	$(CC) $(CFLAGS) -Wno-pointer-arith -Wno-sign-compare -Ishim -I../include \
	    -I$(ATH10K) -o $@ $(QMI_SRCS)

run: glink_test qrtr_test qmi_test
	./glink_test
	./qrtr_test
	./qmi_test

clean:
	rm -f glink_test qrtr_test qmi_test
