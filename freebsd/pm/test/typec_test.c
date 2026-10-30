/*
 * Host test of pro1x_typec.h: the PMI632 Type-C status decoding against
 * Linux 7.1's qcom_pmic_typec_port.c (its get_cc and the MISC_STATUS
 * bits), the VBUS current limit table of qcom_usb_vbus-regulator.c.
 *
 *   cc -I freebsd/pm/sys -o /tmp/typect freebsd/pm/test/typec_test.c && /tmp/typect
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dev/qcom_spmi/pro1x_typec.h"

static int fails;
#define	EQ(a, b) do { long _a = (a), _b = (b); if (_a != _b) { \
	printf("FAIL %d: %s = %ld, want %ld\n", __LINE__, #a, _a, _b); fails++; } } while (0)

int
main(void)
{
	struct typec_state st;
	char buf[128];

	typec_decode(0x00, 0, 0, &st);			/* nothing */
	EQ(st.attached, 0); EQ(st.role, TYPEC_ROLE_NONE);
	/* a PC (Rp default) on CC1, VBUS there: we are its device */
	typec_decode(0x29, 0x08, 0, &st);
	EQ(st.attached, 1); EQ(st.role, TYPEC_ROLE_DEVICE); EQ(st.cc2, 0);
	EQ(st.vbus, 1); EQ(st.rp_ma, 500);
	/* the same flipped, a 3 A charger's Rp */
	typec_decode(0x2b, 0x02, 0, &st);
	EQ(st.cc2, 1); EQ(st.rp_ma, 3000);
	/* an OTG adapter (Rd) on CC2: we source, host role, no VBUS yet */
	typec_decode(0x4b, 0, 0x08, &st);
	EQ(st.role, TYPEC_ROLE_HOST); EQ(st.cc2, 1); EQ(st.vbus, 0);
	/* a powered cable with Rd (Ra on the other CC) */
	typec_decode(0x49, 0, 0x04, &st);
	EQ(st.role, TYPEC_ROLE_HOST);
	/* audio adapter Ra/Ra, debug accessory */
	typec_decode(0x41, 0, 0x01, &st); EQ(st.role, TYPEC_ROLE_AUDIO);
	typec_decode(0x41, 0, 0x10, &st); EQ(st.role, TYPEC_ROLE_DEBUG);
	/* mode names */
	EQ(strcmp(typec_mode_name(0x00), "dual role"), 0);
	EQ(strcmp(typec_mode_name(0x02), "sink only"), 0);
	EQ(strcmp(typec_mode_name(0x04), "source only"), 0);
	EQ(strcmp(typec_mode_name(0x01), "disabled"), 0);
	/* VBUS limits: 500 mA..3 A in 500 mA steps (the Pro1X's DT: 1 A) */
	EQ(typec_vbus_limit(500000), 0); EQ(typec_vbus_limit(1000000), 1);
	EQ(typec_vbus_limit(3000000), 5); EQ(typec_vbus_limit(9000000), 5);
	EQ(typec_vbus_limit(100000), 0);
	typec_decode(0x2b, 0x08, 0, &st);
	typec_format(&st, buf, sizeof(buf));
	EQ(strcmp(buf, "role=device orientation=cc2 vbus=1 rp=500"), 0);
	printf("%s: %d failures\n", fails ? "FAILED" : "OK", fails);
	return (fails != 0);
}
