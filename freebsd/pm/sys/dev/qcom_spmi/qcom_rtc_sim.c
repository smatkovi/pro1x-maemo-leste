/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * QEMU's stand-in for the PMIC RTC (freebsd/pm/test/qemu: PRO1XQ only,
 * not with device qcom_spmi): qcom_rtc.c itself, its register access on a
 * model of the PM6125's RTC and alarm peripherals (0x6000, 0x6100), backed
 * by QEMU virt's PL031 ("arm,pl031"). The PL031's count less
 * hw.qcom_rtc_sim.base (1700000000: a count of some years, as the Pro1X's,
 * not the real time) is the RTC's; the alarm's count, its enable and its
 * clear are the PL031's match register, interrupt mask and interrupt
 * clear. So the PL031's interrupt (SPI 2, INTID 34) is the alarm's: a real
 * device interrupt, which ends a freeze as the SPMI arbiter's (INTID 215)
 * does on the phone. dev.qcom_rtc.0.* as on the phone; with
 * hint.qcom_rtc.0.offset=1700000000 the system clock is the real time.
 */
#define	QCOM_RTC_SIM
#include <dev/qcom_spmi/qcom_rtc.c>

#define	SIM_RTC			0x6000
#define	SIM_ALARM		0x6100
/* the PL031 */
#define	PL031_DR		0x00
#define	PL031_MR		0x04
#define	PL031_IMSC		0x10
#define	PL031_RIS		0x14
#define	PL031_ICR		0x1c

static struct {
	struct resource	*res;
	uint32_t	base;
	uint8_t		alarm[0x100];	/* the alarm peripheral's registers */
} sim;

static u_int sim_base = 1700000000;
SYSCTL_UINT(_hw_qcom_rtc, OID_AUTO, sim_base, CTLFLAG_RDTUN, &sim_base, 0,
    "QEMU: the PL031's count less this is the RTC's");

static uint32_t
sim_count(void)
{
	return (bus_read_4(sim.res, PL031_DR) - sim.base);
}

/* the alarm's enable, count or clear written: the PL031 follows */
static void
sim_alarm_hw(void)
{
	if (sim.alarm[QRTC_ALARM_CTRL] & QRTC_ALARM_EN) {
		bus_write_4(sim.res, PL031_MR,
		    qrtc_le32(&sim.alarm[QRTC_ALARM_RW]) + sim.base);
		bus_write_4(sim.res, PL031_IMSC, 1);
	} else {
		bus_write_4(sim.res, PL031_IMSC, 0);
		bus_write_4(sim.res, PL031_ICR, 1);
	}
}

int
qcom_spmi_read(device_t periph, uint16_t addr, void *buf, size_t len)
{
	uint8_t *p = buf;
	uint32_t c = 0;
	size_t i;

	for (i = 0; i < len; i++, addr++) {
		if (addr >= SIM_RTC + QRTC_RTC_READ &&
		    addr < SIM_RTC + QRTC_RTC_READ + 4) {
			if (i == 0 || addr == SIM_RTC + QRTC_RTC_READ)
				c = sim_count();
			p[i] = c >> (8 * (addr - SIM_RTC - QRTC_RTC_READ));
		} else if (addr == SIM_RTC + QRTC_RTC_CTRL)
			p[i] = QRTC_RTC_EN;
		else if (addr == SIM_RTC + QRTC_PERPH_TYPE)
			p[i] = 0x0b;
		else if (addr == SIM_RTC + QRTC_PERPH_SUBTYPE)
			p[i] = QRTC_SUBTYPE_RTC;
		else if (addr == SIM_ALARM + QRTC_ALARM_CTRL2)
			p[i] = 0;	/* CLEAR reads 0 */
		else if (addr >= SIM_ALARM && addr < SIM_ALARM + 0x100)
			p[i] = sim.alarm[addr - SIM_ALARM];
		else
			return (ENXIO);
	}
	return (0);
}

int
qcom_spmi_write(device_t periph, uint16_t addr, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t i;
	int hw = 0;

	for (i = 0; i < len; i++, addr++) {
		/* the RTC's count cannot be set, as on the Pro1X */
		if (addr >= SIM_RTC && addr < SIM_RTC + 0x100)
			continue;
		if (addr < SIM_ALARM || addr >= SIM_ALARM + 0x100)
			return (ENXIO);
		if (addr == SIM_ALARM + QRTC_ALARM_CTRL2) {
			if (p[i] & QRTC_ALARM_CLEAR)
				bus_write_4(sim.res, PL031_ICR, 1);
			continue;
		}
		sim.alarm[addr - SIM_ALARM] = p[i];
		if (addr == SIM_ALARM + QRTC_ALARM_CTRL ||
		    (addr >= SIM_ALARM + QRTC_ALARM_RW &&
		    addr < SIM_ALARM + QRTC_ALARM_RW + 4))
			hw = 1;
	}
	if (hw)
		sim_alarm_hw();
	return (0);
}

uint16_t
qcom_spmi_base(device_t periph, int index)
{
	return (index == 0 ? SIM_RTC : index == 1 ? SIM_ALARM : 0);
}

static int
qrtc_sim_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) || !ofw_bus_is_compatible(dev, "arm,pl031"))
		return (ENXIO);
	device_set_desc(dev, "Qualcomm PMIC RTC, QEMU's stand-in on the PL031");
	return (BUS_PROBE_SPECIFIC);
}

static int
qrtc_sim_attach(device_t dev)
{
	int rid = 0, error;

	sim.res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sim.res == NULL)
		return (ENXIO);
	sim.base = sim_base;
	sim.alarm[QRTC_PERPH_TYPE] = 0x0b;
	sim.alarm[QRTC_PERPH_SUBTYPE] = QRTC_SUBTYPE_ALARM;
	sim_alarm_hw();
	device_printf(dev, "the PL031's count less %u as the PMIC RTC's\n",
	    sim.base);
	if ((error = qcom_rtc_attach(dev)) != 0)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sim.res);
	return (error);
}

static int
qrtc_sim_detach(device_t dev)
{
	qcom_rtc_detach(dev);
	bus_write_4(sim.res, PL031_IMSC, 0);
	bus_release_resource(dev, SYS_RES_MEMORY, 0, sim.res);
	return (0);
}

static device_method_t qrtc_sim_methods[] = {
	DEVMETHOD(device_probe,		qrtc_sim_probe),
	DEVMETHOD(device_attach,	qrtc_sim_attach),
	DEVMETHOD(device_detach,	qrtc_sim_detach),

	DEVMETHOD(clock_gettime,	qcom_rtc_gettime),
	DEVMETHOD(clock_settime,	qcom_rtc_settime),

	DEVMETHOD_END
};

static driver_t qrtc_sim_driver = {
	"qcom_rtc",
	qrtc_sim_methods,
	sizeof(struct qcom_rtc_softc),
};

DRIVER_MODULE(qcom_rtc, simplebus, qrtc_sim_driver, 0, 0);
