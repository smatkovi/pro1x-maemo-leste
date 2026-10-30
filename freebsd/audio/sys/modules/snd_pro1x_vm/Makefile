# snd_pro1x for the FreeBSD VM's tests (freebsd/audio/test/vm-pcm.sh): the
# same driver without the SMMU and qcom_dsp, attached on nexus with
# hw.snd_pro1x.nexus=1
.PATH: ${SRCTOP}/sys/arm64/qualcomm/audio

KMOD=	snd_pro1x
SRCS=	snd_pro1x.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h opt_platform.h opt_snd.h
SRCS+=	channel_if.h feeder_if.h mixer_if.h
CFLAGS+= -DSND_PRO1X_VM

.include <bsd.kmod.mk>
