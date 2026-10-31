/*
 * The requests and report parsing of the patched libssc
 * (patches/libssc-wakeup-batching.patch), for check.py, which decodes them
 * against the sensor core's field numbers (see-client.proto). Built from
 * libssc's src/libssc-request.c and its generated data/ssc-common.pb-c.c,
 * without the rest of libssc (no QMI, no sensor core needed).
 *
 *   encode requests            one line per case: NAME HEX
 *   encode defaults TYPE:C...  libssc.conf's delivery for each data type
 *                              (C: 1 streaming, 0 on-change):
 *                              TYPE WAKEUP BATCH_US
 *   encode count < HEX lines   the step count in each report, or "fail"
 *   encode events < HEX lines  a whole report (SscClientResponse): per
 *                              event MSG_ID COUNT|-
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "ssc-common.pb-c.h"
#include "libssc-request-private.h"

#define UID_HIGH 0x1122334455667788ULL
#define UID_LOW  0x99aabbccddeeff00ULL

static void put_hex(const char *name, const GArray *a)
{
	printf("%s ", name);
	for (guint i = 0; i < a->len; i++)
		printf("%02x", (guint8)a->data[i]);
	printf("\n");
}

static void request(const char *name, guint32 msg_id, const GArray *payload,
		    const SSCDelivery *d)
{
	GArray *a = ssc_request_pack(UID_HIGH, UID_LOW, msg_id,
				     payload ? (const guint8 *)payload->data : NULL,
				     payload ? payload->len : 0, d);
	put_hex(name, a);
	g_array_free(a, TRUE);
}

static GArray *sample_rate(float hz)
{
	SscEnableConfigRequest m;
	GArray *a = g_array_new(FALSE, FALSE, 1);

	ssc_enable_config_request__init(&m);
	m.sample_rate = hz;
	g_array_set_size(a, ssc_enable_config_request__get_packed_size(&m));
	ssc_enable_config_request__pack(&m, (unsigned char *)a->data);
	return a;
}

static GArray *unhex(const char *s)
{
	GArray *a = g_array_new(FALSE, FALSE, 1);
	size_t n = strlen(s);

	for (size_t i = 0; i + 1 < n; i += 2) {
		char b[3] = { s[i], s[i + 1], 0 };
		guint8 v = (guint8)strtoul(b, NULL, 16);
		g_array_append_val(a, v);
	}
	return a;
}

int main(int argc, char **argv)
{
	char line[65536];

	if (argc >= 2 && !strcmp(argv[1], "requests")) {
		GArray *rate = sample_rate(13.0f);
		SSCDelivery legacy = SSC_DELIVERY_DEFAULT;
		SSCDelivery accel = { FALSE, 200000, 0, FALSE };
		SSCDelivery steps = { FALSE, 5000000, 0, FALSE };
		SSCDelivery idle = { FALSE, 600000000, 900000000, TRUE };
		SSCDelivery small_flush = { FALSE, 400000, 100000, FALSE };
		SSCDelivery prox = { TRUE, 0, 0, FALSE };

		request("legacy-null", 513, rate, NULL);
		request("legacy-default", 513, rate, &legacy);
		request("accel-batched", 513, rate, &accel);
		request("pedometer", 514, NULL, &steps);
		request("pedometer-idle", 514, NULL, &idle);
		request("flush-period-raised", 513, rate, &small_flush);
		request("proximity", 514, NULL, &prox);
		request("flush", 2, NULL, &steps);
		request("disable", 10, NULL, NULL);
		g_array_free(rate, TRUE);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "defaults")) {
		for (int i = 2; i < argc; i++) {
			char *type = g_strdup(argv[i]), *c = strchr(type, ':');
			SSCDelivery d;

			if (c)
				*c++ = 0;
			ssc_request_delivery_defaults(type, c && *c == '1', &d);
			printf("%s %d %u\n", type, d.wakeup ? 1 : 0, d.batch_period_us);
			g_free(type);
		}
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "count")) {
		while (fgets(line, sizeof(line), stdin)) {
			GArray *a = unhex(g_strstrip(line));
			guint count;

			if (ssc_report_parse_count((const guint8 *)a->data, a->len, &count))
				printf("%u\n", count);
			else
				printf("fail\n");
			g_array_free(a, TRUE);
		}
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "events")) {
		while (fgets(line, sizeof(line), stdin)) {
			GArray *a = unhex(g_strstrip(line));
			SscClientResponse *r = ssc_client_response__unpack(NULL, a->len,
									  (const uint8_t *)a->data);

			if (!r) {
				printf("fail\n");
			} else {
				printf("uid %016" G_GINT64_MODIFIER "x %016" G_GINT64_MODIFIER "x",
				       r->uid->high, r->uid->low);
				for (size_t i = 0; i < r->n_response; i++) {
					guint count;

					if (ssc_report_parse_count(r->response[i]->msg.data,
								   r->response[i]->msg.len, &count))
						printf(" %u:%u", r->response[i]->msg_id, count);
					else
						printf(" %u:-", r->response[i]->msg_id);
				}
				printf("\n");
				ssc_client_response__free_unpacked(r, NULL);
			}
			g_array_free(a, TRUE);
		}
		return 0;
	}
	fprintf(stderr, "usage: encode requests|defaults TYPE:C...|count|events\n");
	return 2;
}
