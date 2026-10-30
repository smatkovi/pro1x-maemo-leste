/*
 * The SMS codec (pro1x-modemd/sms.c) against known PDUs and round trips:
 * KolibriOS for ARM's tests/modem test_sms(), with the zone and the class
 * of the data coding scheme, which pro1x-modemd passes on to oFono's
 * clients.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>

#include "../pro1x-modemd/sms.h"

static int failed, passed;
#define	CHECK(c, ...) do { if (c) passed++; else { failed++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static uint32_t
hexpdu(const char *h, uint8_t *out)
{
	uint32_t n = 0;

	for (; h[0] && h[1]; h += 2) {
		unsigned v;

		sscanf(h, "%2x", &v);
		out[n++] = v;
	}
	return n;
}

int
main(void)
{
	struct sms_part p;
	uint8_t pdu[200];
	/* the classic example (SMSC +31624000000) */
	uint32_t n = hexpdu("07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07", pdu);

	CHECK(sms_decode(pdu, n, &p) == 0, "decode the example");
	CHECK(!strcmp(p.number, "+31641600986"), "number %s", p.number);
	CHECK(!strcmp(p.text, "How are you?"), "text '%s'", p.text);
	CHECK(p.time == 1030390661u, "time %u", p.time);
	CHECK(p.tz_minutes == 0, "zone %d min (the octet 08: -0)", p.tz_minutes);
	CHECK(!p.submit && !p.total, "single DELIVER");
	CHECK(!sms_class0(&p), "not class 0");

	/* UCS-2 by hand: "Grüße" from +4366 */
	n = hexpdu("0004049166660008" "62019221000040" "0A004700720" "0FC00DF0065", pdu);
	CHECK(sms_decode(pdu, n, &p) == 0, "decode UCS-2");
	CHECK(!strcmp(p.text, "Gr\xc3\xbc\xc3\x9f" "e"), "UCS-2 text '%s'", p.text);
	CHECK(p.time == 1793271600u, "UCS-2 time %u (2026-10-29 12:00 +01)", p.time);
	CHECK(p.tz_minutes == 60, "UCS-2 zone %d min", p.tz_minutes);

	/* the same with a negative zone (bit 3 of the zone octet): -05:00 */
	n = hexpdu("0004049166660008" "620192210000" "0A" "0A004700720" "0FC00DF0065", pdu);
	CHECK(sms_decode(pdu, n, &p) == 0 && p.tz_minutes == -300,
	    "zone -5 h: %d min", p.tz_minutes);
	CHECK(p.time == 1793271600u + 6 * 3600, "UTC of 12:00 -05 (%u)", p.time);

	/* alphanumeric sender "Info" */
	n = hexpdu("000407D049B7F90D0000" "62019221000040" "03" "C87408", pdu);
	CHECK(sms_decode(pdu, n, &p) == 0 && !strcmp(p.number, "Info") &&
	    !strcmp(p.text, "Hi!"), "alphanumeric '%s': '%s'", p.number, p.text);

	/* class 0 (flash): DCS 0x10 (general group, class 0, 7-bit) */
	n = hexpdu("000407D049B7F90D0010" "62019221000040" "03" "C87408", pdu);
	CHECK(sms_decode(pdu, n, &p) == 0 && sms_class0(&p) && !strcmp(p.text, "Hi!"),
	    "class 0 '%s'", p.text);
	/* DCS 0x11: class 1, not flash */
	n = hexpdu("000407D049B7F90D0011" "62019221000040" "03" "C87408", pdu);
	CHECK(sms_decode(pdu, n, &p) == 0 && !sms_class0(&p), "class 1");

	/* a status report is no message */
	n = hexpdu("0006" "0B" "0B911346610089F6" "62019221000040" "62019221000040" "00", pdu);
	CHECK(sms_decode(pdu, n, &p) == -1, "status report refused");

	/* round trips through SUBMIT */
	static uint8_t out[6][SMS_PDU_MAX];
	uint32_t len[6];
	const char *texts[] = {
		"Hallo Welt",
		"Preis: 5\xe2\x82\xac [ok] {x} ^~|\\",
		"Gr\xc3\xbc\xc3\x9f" "e aus Wien \xf0\x9f\x99\x82",
		"@\xc2\xa3$ \xc3\xa4\xc3\xb6\xc3\xbc \xc3\x84\xc3\x96\xc3\x9c \xc3\x9f \xce\x94\xce\xa9",
	};
	for (unsigned i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
		int parts = sms_encode("+436641234567", texts[i], 9, out, len, 6);

		CHECK(parts == 1, "'%s' in one part (%d)", texts[i], parts);
		CHECK(sms_decode(out[0], len[0], &p) == 0 && p.submit, "decode SUBMIT %u", i);
		CHECK(!strcmp(p.text, texts[i]), "round trip '%s' -> '%s'", texts[i], p.text);
		CHECK(!strcmp(p.number, "+436641234567"), "SUBMIT number %s", p.number);
	}
	CHECK(out[0][0] == 0 && (out[0][1] & 3) == 1, "default SMSC, SUBMIT");

	/* concatenation: 400 GSM characters, 3 parts */
	char longtext[420], joined[1200] = "";
	for (int i = 0; i < 400; i++)
		longtext[i] = 'a' + i % 26;
	longtext[400] = 0;
	longtext[152] = '{';	/* an escape across the part border */
	int parts = sms_encode("0664123", longtext, 77, out, len, 6);
	CHECK(parts == 3, "400 characters in %d parts", parts);
	for (int i = 0; i < parts; i++) {
		CHECK(sms_decode(out[i], len[i], &p) == 0, "decode part %d", i);
		CHECK(p.ref == 77 && p.total == 3 && p.seq == i + 1,
		    "part %d: ref %u total %u seq %u", i, p.ref, p.total, p.seq);
		strcat(joined, p.text);
	}
	CHECK(!strcmp(joined, longtext), "joined parts (%zu vs %zu)", strlen(joined),
	    strlen(longtext));
	CHECK(!strcmp(p.number, "0664123"), "national number %s", p.number);
	/* 80 non-GSM characters: UCS-2 in 2 parts */
	char u[400] = "";
	for (int i = 0; i < 80; i++)
		strcat(u, "\xd0\x96");	/* Ж */
	CHECK(sms_encode("+43", u, 1, out, len, 6) == 2, "UCS-2 in 2 parts");
	/* too long for the parts allowed */
	CHECK(sms_encode("+43", longtext, 1, out, len, 2) == 0, "3 parts refused at max 2");

	printf("test_sms: %d passed, %d failed\n", passed, failed);
	return failed != 0;
}
