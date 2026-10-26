# Host tests of the DSP stack's protocol code (freebsd/dsp/README.md):
#   make -C freebsd/dsp/test
SRC=	../sys/arm64/qualcomm/dsp
CFLAGS?=	-O2 -g
CFLAGS+=	-Wall -Wextra -Wno-unused-parameter -I$(SRC) -pthread -fsanitize=address,undefined

all: run

glink_test: glink_test.c $(SRC)/glink_core.c $(SRC)/glink.h $(SRC)/glink_os.h
	$(CC) $(CFLAGS) -o $@ glink_test.c $(SRC)/glink_core.c

run: glink_test
	./glink_test

clean:
	rm -f glink_test
