# Host tests of the camera stack (freebsd/camera/README.md): make
CC?=	cc
CFLAGS+=	-O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
	-Wno-unused-const-variable -Iinclude -fsanitize=address,undefined
K=	../sys/arm64/qualcomm/camss
U=	../tools/pro1x-cam

all: test_cci test_calc
	./test_cci
	./test_calc

test_cci: test_cci.c $(K)/qcom_cci.c cci_host_shim.h
	$(CC) $(CFLAGS) -I. -o $@ test_cci.c

test_calc: test_calc.c $(K)/camss_gcc.c $(K)/camss_stream.c $(U)/calc.c
	$(CC) $(CFLAGS) -o $@ test_calc.c $(K)/camss_gcc.c $(K)/camss_stream.c $(U)/calc.c

clean:
	rm -f test_cci test_calc test_image *.o
