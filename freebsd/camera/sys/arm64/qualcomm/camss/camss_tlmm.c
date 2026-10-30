/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The TLMM pins the camera needs, set from the device tree's pinctrl
 * states as Linux's pinctrl-msm does it: FreeBSD's qcom_tlmm_sm6115
 * (patches/0006) has no pin control, and the boot menu leaves the CCI and
 * MCLK pins in their GPIO function. Each pin's CTL register (0x1000 *
 * gpio of its tile): bias bits 0-1, function bits 2-5 (the index of the
 * state's "function" in the pin's list, camss_tlmm_sm6115.h from
 * pinctrl-sm6115.c), drive bits 6-8 ((mA / 2) - 1), OE bit 9; IO at +4:
 * out bit 1. Only pins of the states asked for are touched; the GPIO
 * driver's own pins (keys, touch) are not among them.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/sx.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/bus.h>
#include <machine/cpufunc.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>

#include "camss_var.h"
#include "camss_tlmm_sm6115.h"

#define	CTL_PULL_MASK		0x3u
#define	CTL_FUNC_SHIFT		2
#define	CTL_FUNC_MASK		(0xfu << 2)
#define	CTL_DRV_SHIFT		6
#define	CTL_DRV_MASK		(0x7u << 6)
#define	CTL_OE			(1u << 9)
#define	IO_OUT			(1u << 1)

static struct {
	struct sx	lock;
	int		state;
	phandle_t	node;
	vm_paddr_t	tile[3];
	volatile uint32_t *pin[TLMM_NPINS];	/* CTL page of each pin */
} tlmm;
SX_SYSINIT(camss_tlmm_lock, &tlmm.lock, "camss_tlmm");

static int
tlmm_attach(void)
{
	static const char *const names[3] = { "west", "south", "east" };
	bus_addr_t pa;
	bus_size_t size;
	phandle_t node;
	char *rn = NULL;
	ssize_t len, off;
	int i, idx;

	if (tlmm.state != 0)
		return (tlmm.state == 1 ? 0 : tlmm.state);
	node = ofw_bus_find_compatible(OF_finddevice("/"), "qcom,sm6115-tlmm");
	if (node <= 0) {
		tlmm.state = ENXIO;
		return (ENXIO);
	}
	tlmm.node = node;
	/* the tiles by reg-names (the driver of 0006 assumes this order) */
	len = OF_getprop_alloc(node, "reg-names", (void **)&rn);
	for (i = 0; i < 3; i++) {
		idx = i;
		for (off = 0, idx = -1; rn != NULL && off < len;
		    off += strlen(rn + off) + 1) {
			idx++;
			if (strcmp(rn + off, names[i]) == 0)
				break;
		}
		if (rn == NULL || off >= len)
			idx = i;
		if (ofw_reg_to_paddr(node, idx, &pa, &size, NULL) != 0) {
			OF_prop_free(rn);
			tlmm.state = ENXIO;
			return (ENXIO);
		}
		tlmm.tile[i] = pa;
	}
	OF_prop_free(rn);
	tlmm.state = 1;
	return (0);
}

static volatile uint32_t *
ctl(u_int pin)
{
	if (tlmm.pin[pin] == NULL)
		tlmm.pin[pin] = pmap_mapdev(tlmm.tile[tlmm_pins[pin].tile] +
		    0x1000 * (vm_paddr_t)pin, PAGE_SIZE);
	return (tlmm.pin[pin]);
}

int
camss_gpio_read(u_int pin, uint32_t *c, uint32_t *io)
{
	int error;

	sx_xlock(&tlmm.lock);
	if ((error = tlmm_attach()) == 0 && pin < TLMM_NPINS) {
		*c = ctl(pin)[0];
		*io = ctl(pin)[1];
	} else if (error == 0)
		error = EINVAL;
	sx_xunlock(&tlmm.lock);
	return (error);
}

/* a pin in its GPIO function as an output at value (the IO first) */
int
camss_gpio_out(u_int pin, int value, const char *who)
{
	volatile uint32_t *p;
	uint32_t c0, c;
	int error;

	sx_xlock(&tlmm.lock);
	if ((error = tlmm_attach()) != 0 || pin >= TLMM_NPINS) {
		sx_xunlock(&tlmm.lock);
		return (error ? error : EINVAL);
	}
	p = ctl(pin);
	c0 = p[0];
	p[1] = value ? (p[1] | IO_OUT) : (p[1] & ~IO_OUT);
	dsb(sy);
	c = (c0 & ~CTL_FUNC_MASK) | CTL_OE;
	p[0] = c;
	dsb(sy);
	sx_xunlock(&tlmm.lock);
	if (c != c0)
		printf("camss_tlmm: %s: gpio%u CTL %#x -> %#x\n", who, pin, c0, c);
	return (0);
}

static int
func_index(u_int pin, const char *fn)
{
	int i;

	for (i = 0; i < 10; i++)
		if (tlmm_pins[pin].func[i] != NULL &&
		    strcmp(tlmm_pins[pin].func[i], fn) == 0)
			return (i);
	return (-1);
}

/* one state node with "pins": msm_pinmux_set_mux + msm_config_group_set */
static int
apply_state(phandle_t st, const char *who)
{
	char *pins = NULL, fn[32];
	ssize_t len, off;
	uint32_t c0, c, drive;
	volatile uint32_t *p;
	u_int pin;
	int f, error = 0;

	if ((len = OF_getprop_alloc(st, "pins", (void **)&pins)) <= 0)
		return (0);
	if (OF_getprop(st, "function", fn, sizeof(fn)) <= 0)
		fn[0] = '\0';
	for (off = 0; off < len; off += strlen(pins + off) + 1) {
		if (sscanf(pins + off, "gpio%u", &pin) != 1 || pin >= TLMM_NPINS) {
			printf("camss_tlmm: %s: pin %s not handled\n", who,
			    pins + off);
			error = EINVAL;
			continue;
		}
		sx_xlock(&tlmm.lock);
		p = ctl(pin);
		c0 = c = p[0];
		if (fn[0] != '\0') {
			if ((f = func_index(pin, fn)) < 0) {
				sx_xunlock(&tlmm.lock);
				printf("camss_tlmm: %s: gpio%u has no function "
				    "%s\n", who, pin, fn);
				error = EINVAL;
				continue;
			}
			c = (c & ~CTL_FUNC_MASK) | (uint32_t)f << CTL_FUNC_SHIFT;
		}
		if (OF_getencprop(st, "drive-strength", &drive,
		    sizeof(drive)) > 0 && drive >= 2 && drive <= 16)
			c = (c & ~CTL_DRV_MASK) |
			    ((drive / 2 - 1) << CTL_DRV_SHIFT);
		if (OF_hasprop(st, "bias-disable"))
			c &= ~CTL_PULL_MASK;
		else if (OF_hasprop(st, "bias-pull-down"))
			c = (c & ~CTL_PULL_MASK) | 1;
		else if (OF_hasprop(st, "bias-bus-hold"))
			c = (c & ~CTL_PULL_MASK) | 2;
		else if (OF_hasprop(st, "bias-pull-up"))
			c = (c & ~CTL_PULL_MASK) | 3;
		if (OF_hasprop(st, "output-high") || OF_hasprop(st, "output-low")) {
			p[1] = OF_hasprop(st, "output-high") ?
			    (p[1] | IO_OUT) : (p[1] & ~IO_OUT);
			c |= CTL_OE;
		} else if (OF_hasprop(st, "input-enable"))
			c &= ~CTL_OE;
		p[0] = c;
		dsb(sy);
		sx_xunlock(&tlmm.lock);
		printf("camss_tlmm: %s: gpio%u %s: CTL %#x -> %#x\n", who, pin,
		    fn[0] ? fn : "(config)", c0, c);
	}
	OF_prop_free(pins);
	return (error);
}

/* the node's pinctrl-<idx> states (each a node with pins, or its children) */
int
camss_pinctrl(phandle_t node, int idx, const char *who)
{
	char prop[16];
	pcell_t *ph = NULL;
	phandle_t st, ch;
	ssize_t n;
	int error, i, e;

	if ((error = tlmm_attach()) != 0)
		return (error);
	snprintf(prop, sizeof(prop), "pinctrl-%d", idx);
	n = OF_getencprop_alloc_multi(node, prop, sizeof(pcell_t), (void **)&ph);
	for (i = 0, error = 0; i < n; i++) {
		st = OF_node_from_xref(ph[i]);
		if (OF_parent(st) != tlmm.node && OF_parent(OF_parent(st)) !=
		    tlmm.node) {
			printf("camss_tlmm: %s: state not of the TLMM\n", who);
			error = ENXIO;
			continue;
		}
		if (OF_hasprop(st, "pins"))
			e = apply_state(st, who);
		else
			for (ch = OF_child(st), e = 0; ch > 0; ch = OF_peer(ch))
				e = apply_state(ch, who) ? EINVAL : e;
		if (e != 0)
			error = e;
	}
	OF_prop_free(ph);
	return (error);
}

/* <&tlmm pin flags> of prop: the pin and whether it is active low */
int
camss_dt_gpio(phandle_t node, const char *prop, int *pin, int *active_low)
{
	pcell_t c[3];
	int error;

	*pin = -1;
	*active_low = 0;
	if (OF_getencprop(node, prop, c, sizeof(c)) != sizeof(c))
		return (ENOENT);
	sx_xlock(&tlmm.lock);
	error = tlmm_attach();
	sx_xunlock(&tlmm.lock);
	if (error != 0)
		return (error);
	if (OF_node_from_xref(c[0]) != tlmm.node || c[1] >= TLMM_NPINS)
		return (ENXIO);
	*pin = c[1];
	*active_low = (c[2] & 1) != 0;		/* GPIO_ACTIVE_LOW */
	return (0);
}
