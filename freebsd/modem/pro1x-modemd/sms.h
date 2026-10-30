/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * SMS PDUs (3GPP TS 23.040, 23.038): SMS-DELIVER and SMS-SUBMIT, the text
 * in UTF-8. KolibriOS for ARM's kernel/modem/sms.c (github.com/smatkovi/
 * kolibrios-arm, GPL-2.0, tested there against known PDUs), here with
 * <stdint.h> types and the time stamp's zone kept (for oFono's
 * LocalSentTime).
 */

#ifndef _PRO1X_SMS_H_
#define	_PRO1X_SMS_H_

#include <stdbool.h>
#include <stdint.h>

#define	SMS_PDU_MAX	180	/* SMSC address and TPDU */
#define	SMS_TEXT_MAX	680	/* UTF-8 of one part (70 UCS-2 characters, 4 bytes each) */

struct sms_part {
	char		number[32];	/* the sender (DELIVER), the recipient (SUBMIT) */
	uint32_t	time;		/* the service centre's time stamp, Unix (UTC); 0: none */
	int		tz_minutes;	/* the time stamp's zone, minutes east of UTC */
	char		text[SMS_TEXT_MAX];
	uint16_t	ref;		/* concatenation: reference, parts, this part (1..) */
	uint8_t		total, seq;
	bool		submit;		/* an SMS-SUBMIT (sent, stored on the modem) */
	uint8_t		dcs;		/* the data coding scheme (class 0: 0x10 or 0xf0 set, bits 0-1 = 0) */
};

/*
 * A PDU as the modem stores and delivers it (the SMSC address in front);
 * 0, or -1 for what is not a message (a status report, ...).
 */
int sms_decode(const uint8_t *pdu, uint32_t len, struct sms_part *p);

/*
 * Text to number as SMS-SUBMIT PDUs (the default SMSC), one per part
 * (concatenated beyond 160 GSM characters or 70 UCS-2 ones): out[i] of
 * len[i] bytes; the number of parts, 0 when it does not fit max.
 */
int sms_encode(const char *number, const char *utf8, uint8_t ref,
    uint8_t out[][SMS_PDU_MAX], uint32_t *len, int max);

/* a class 0 ("flash", oFono's ImmediateMessage) message */
bool sms_class0(const struct sms_part *p);

/* UTF-8 helpers */
uint32_t utf8_get(const char **s);	/* the next code point (0: end) */
int utf8_put(char *d, uint32_t cp);	/* bytes written (up to 4) */

#endif /* _PRO1X_SMS_H_ */
