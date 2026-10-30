# Host tests of FreeBSD's sound (freebsd/audio/README.md, "Tests on the PC"):
#   make -C freebsd/audio/test
CC ?= cc
CFLAGS = -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-fsanitize=address,undefined -fno-sanitize-recover=all -Ishim
D = ../tools/pro1x-audiod
FM = ../../fm/tools/pro1x-fmaudio
SRCS = test_audiod.c $(D)/apr.c $(D)/q6.c $(D)/pcm.c $(D)/codec.c \
	$(D)/swr.c $(D)/voice.c $(D)/backend.c $(D)/mbhc.c $(FM)/afe.c $(FM)/mmio.c

all: test_audiod trace2seq
	python3 -I ../tools/trace2seq.py --sample > sample.seq
	./test_audiod sample.seq ../seq/base.seq > test_audiod.log 2>&1 || \
	    { cat test_audiod.log; exit 1; }
	grep -E 'done|passed|FAIL' test_audiod.log

test_audiod: $(SRCS) $(D)/audiod.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

trace2seq:
	python3 -I ../tools/trace2seq.py --selftest

clean:
	rm -f test_audiod test_audiod.log sample.seq

.PHONY: all clean trace2seq
