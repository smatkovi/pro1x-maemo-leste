# Host tests of the IPA's code (freebsd/data/README.md):
#   make -C freebsd/data/test [LINUX=<linux tree>]
# Linux's own IPA tables are compiled from LINUX (read only).
IPA=	../sys/arm64/qualcomm/ipa
DSP=	../../dsp/sys/arm64/qualcomm/dsp
LINUX?=	../../../../build/linux
LIPA=	$(LINUX)/drivers/net/ipa
CFLAGS?=	-O1 -g
CFLAGS+=	-Wall -Wextra -Wno-unused-parameter -pthread \
		-fsanitize=address,undefined -fno-omit-frame-pointer
LXFLAGS=	-Ishim -I$(LIPA) -w

all: run

test_regs: test_regs.c my_regs.c linux_regs.c regs.def $(IPA)/ipa_regs.h
	$(CC) $(CFLAGS) -I$(IPA) -c -o my_regs.o my_regs.c
	$(CC) $(CFLAGS) $(LXFLAGS) -I. -c -o linux_regs.o linux_regs.c
	$(CC) $(CFLAGS) $(LXFLAGS) -c -o lx_ipa_reg.o $(LIPA)/reg/ipa_reg-v4.2.c
	$(CC) $(CFLAGS) $(LXFLAGS) -c -o lx_gsi_reg.o $(LIPA)/reg/gsi_reg-v4.0.c
	$(CC) $(CFLAGS) -o $@ test_regs.c my_regs.o linux_regs.o lx_ipa_reg.o \
	    lx_gsi_reg.o

test_qmap: test_qmap.c $(IPA)/qmap.c $(IPA)/qmap.h $(IPA)/ipa_regs.h
	$(CC) $(CFLAGS) -I$(IPA) -Ishim -iquote $(LINUX)/include/linux -o $@ \
	    test_qmap.c $(IPA)/qmap.c

GSI_SRCS=	test_gsi.c $(IPA)/gsi.c $(IPA)/ipa_hw.c $(IPA)/qmap.c
test_gsi: $(GSI_SRCS) $(IPA)/ipa_var.h $(IPA)/ipa_os.h $(IPA)/ipa_regs.h
	$(CC) $(CFLAGS) -I$(IPA) -o $@ $(GSI_SRCS)

# the IPA's QMI on freebsd/dsp's QMI and QRTR (as dsp/test/qmi_test)
DSPT=	../../dsp/test
LXSYMS=	ipa_indication_register_req_ei ipa_indication_register_rsp_ei \
	ipa_driver_init_complete_req_ei ipa_driver_init_complete_rsp_ei \
	ipa_init_complete_ind_ei ipa_mem_bounds_ei ipa_mem_array_ei \
	ipa_mem_range_ei ipa_init_modem_driver_req_ei ipa_init_modem_driver_rsp_ei
LXRENAME=	$(foreach s,$(LXSYMS),-D$(s)=lx_$(s))
QMI_SRCS=	test_qmi.c $(IPA)/ipa_qmi.c $(IPA)/ipa_qmi_msg.c $(DSP)/qmi_core.c \
		$(DSP)/qmi_encdec.c $(DSP)/qrtr_core.c
test_qmi: $(QMI_SRCS) $(IPA)/ipa_qmi_msg.h $(IPA)/ipa_var.h
	$(CC) $(CFLAGS) -w -Ishim -I$(DSPT)/shim -I$(DSP) $(LXRENAME) \
	    -c -o lx_ipa_qmi_msg.o $(LIPA)/ipa_qmi_msg.c
	$(CC) $(CFLAGS) -Wno-pointer-arith -Wno-sign-compare -Wno-cast-qual \
	    -I$(IPA) -I$(DSP) -Ishim -I$(DSPT)/shim -o $@ $(QMI_SRCS) \
	    lx_ipa_qmi_msg.o

run: test_regs test_qmap test_gsi test_qmi
	./test_regs
	./test_qmap
	./test_gsi
	./test_qmi

clean:
	rm -f test_regs *.o
