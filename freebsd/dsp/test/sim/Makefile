# The DSP stack's start on the PC (freebsd/dsp/README.md, "The simulation"):
#   make -C freebsd/dsp/test/sim         build, run every scenario
# FreeBSD's driver files compile unchanged against simkern.h (the kernel's
# interface on POSIX threads), under ASan and UBSan. The device tree is
# pro1x-dsp.dts; the Pro1X's real one (DTB=...) is used too when found.
SRC=	../../sys/arm64/qualcomm/dsp
TOOLS=	../../tools
DTB?=	../../../../../out/sm6115-fxtec-pro1x.dtb
CC?=	cc
SAN=	-fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS?= -O1 -g
CFLAGS+= -Wall -Wno-unused-parameter -Wno-unused-function -pthread $(SAN)
KFLAGS=	-D_KERNEL -DQCOM_DSP_SIM -Iinclude -I. -I$(SRC) -include simkern.h \
	-Wno-address-of-packed-member -Wno-format-truncation
# the drivers of qcom_dsp.ko and qrtr.ko that the start goes through
KSRCS=	$(SRC)/qcom_dsp.c $(SRC)/qcom_scm.c $(SRC)/qcom_smem.c \
	$(SRC)/qcom_ipc.c $(SRC)/qcom_smp2p.c $(SRC)/qcom_pas.c \
	$(SRC)/glink_core.c $(SRC)/qcom_glink.c $(SRC)/qcom_rmtfs.c \
	$(SRC)/qrtr_core.c $(SRC)/qrtr_glink.c $(SRC)/qrtr_mod.c
SIMSRCS= simkern.c fdt.c sim.c loaderglue.c
HWSRCS=	hw.c remote.c simfw.c
# the loader, its system calls into the simulation
LFLAGS=	-I$(SRC) -Dmain=pro1x_dsp_main -Dopen=sim_open -Dclose=sim_close \
	-Dioctl=sim_ioctl -Derr=sim_err -Derrx=sim_errx -Dwarn=sim_warn \
	-Dwarnx=sim_warnx -Dexit=sim_exit -Dprintf=sim_printf

all: run

sim: $(KSRCS) $(SIMSRCS) $(HWSRCS) $(TOOLS)/pro1x-dsp/pro1x-dsp.c *.h \
    include/sys/*.h $(SRC)/*.h
	rm -rf obj && mkdir obj
	for f in $(KSRCS) $(SIMSRCS); do \
		$(CC) $(CFLAGS) $(KFLAGS) -c -o obj/$$(basename $$f .c).o $$f || exit 1; \
	done
	for f in $(HWSRCS); do \
		$(CC) $(CFLAGS) -I. -c -o obj/$$(basename $$f .c).o $$f || exit 1; \
	done
	$(CC) $(CFLAGS) $(LFLAGS) -c -o obj/pro1x-dsp.o $(TOOLS)/pro1x-dsp/pro1x-dsp.c
	$(CC) $(CFLAGS) -o $@ obj/*.o

pro1x-dsp.dtb: pro1x-dsp.dts
	dtc -q -I dts -O dtb -o $@ pro1x-dsp.dts

run: sim pro1x-dsp.dtb
	./sim -q dsp
	./sim -q ready-early
	./sim -q no-irq
	./sim -q adopt
	@if [ -f $(DTB) ]; then echo ">> the Pro1X's own device tree: $(DTB)"; \
	    ./sim -q -d $(DTB) dsp && ./sim -q -d $(DTB) adopt; fi

clean:
	rm -rf obj sim pro1x-dsp.dtb
