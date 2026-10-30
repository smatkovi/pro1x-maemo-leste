# The Pro1X's sound as a sound(4) device, its PCM moved by pro1x-audiod
# through the ADSP (/dev/pro1x_pcm, freebsd/audio/README.md). Built with
# -DSND_PRO1X_VM (no SMMU, no qcom_dsp) for the FreeBSD VM's tests.
.PATH: ${SRCTOP}/sys/arm64/qualcomm/audio

KMOD=	snd_pro1x
SRCS=	snd_pro1x.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h opt_platform.h opt_snd.h
SRCS+=	channel_if.h feeder_if.h mixer_if.h
CFLAGS+= -I${SRCTOP}/sys/arm64/qualcomm/dsp

.include <bsd.kmod.mk>
