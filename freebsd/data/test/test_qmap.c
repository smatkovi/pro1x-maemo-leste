/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * qmap.c on the PC: the formats against Linux's include/linux/if_rmnet.h
 * (compiled from the Linux tree), the GSI's and the immediate commands'
 * layouts against Linux's gsi.c, gsi_trans.c and ipa_cmd.c, and packets
 * built here with a checksum routine of their own (RFC 1071, not qmap.c's):
 * IPv4 TCP and UDP (with and without checksum), IPv6 TCP and UDP, padding,
 * a MAP command, a payload changed on the way, a bad IPv4 header, a
 * fragment, ICMP, a trailer without its valid flag, an empty frame and a
 * truncated one; the uplink headers as rmnet writes them.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <linux/types.h>
#include <linux/bits.h>
#include "if_rmnet.h"		/* Linux's, from $(LINUX)/include/linux */

#include "qmap.h"
#include "ipa_regs.h"

/* --- packets ------------------------------------------------------------------------ */

/* RFC 1071 over bytes in network order, independent of qmap.c */
static uint16_t
rfc1071(const uint8_t *p, size_t len, uint32_t start)
{
	uint32_t sum = start;
	size_t i;

	for (i = 0; i + 1 < len; i += 2)
		sum += (uint32_t)p[i] << 8 | p[i + 1];
	if (len & 1)
		sum += (uint32_t)p[len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ((uint16_t)sum);	/* the folded sum, not complemented */
}

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v;
}

static const uint8_t src4[4] = { 10, 64, 1, 2 }, dst4[4] = { 1, 1, 1, 1 };
static const uint8_t src6[16] = { 0x2a, 0x02, 0x83, 0x88, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0x42 };
static const uint8_t dst6[16] = { 0x26, 0x06, 0x47, 0x00, 0x47, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0x11, 0x11 };

/* An IPv4 packet with a TCP (proto 6) or UDP (17) or ICMP (1) payload */
static size_t
ipv4(uint8_t *p, uint8_t proto, size_t plen, bool udp_csum, uint16_t frag)
{
	size_t thl = proto == 6 ? 20 : 8, len = 20 + thl + plen, i;
	uint32_t ps;
	uint8_t *t = p + 20;

	memset(p, 0, len);
	p[0] = 0x45;
	put16(p + 2, len);
	put16(p + 4, 0x1234);
	put16(p + 6, frag);
	p[8] = 64;
	p[9] = proto;
	memcpy(p + 12, src4, 4);
	memcpy(p + 16, dst4, 4);
	put16(p + 10, ~rfc1071(p, 20, 0));
	put16(t, 40000);
	put16(t + 2, proto == 6 ? 443 : 53);
	for (i = 0; i < plen; i++)
		t[thl + i] = (uint8_t)(i * 7 + 3);
	if (proto == 6) {
		t[12] = 5 << 4;
		t[13] = 0x18;
	} else if (proto == 17)
		put16(t + 4, thl + plen);
	if (proto == 1)
		return (len);
	ps = rfc1071(p + 12, 8, 0) + proto + thl + plen;
	if (proto == 6)
		put16(t + 16, ~rfc1071(t, thl + plen, ps));
	else if (udp_csum)
		put16(t + 6, ~rfc1071(t, thl + plen, ps));
	return (len);
}

static size_t
ipv6(uint8_t *p, uint8_t nxt, size_t plen)
{
	size_t thl = nxt == 6 ? 20 : 8, len = 40 + thl + plen, i;
	uint32_t ps;
	uint8_t *t = p + 40;

	memset(p, 0, len);
	p[0] = 0x60;
	put16(p + 4, thl + plen);
	p[6] = nxt;
	p[7] = 64;
	memcpy(p + 8, src6, 16);
	memcpy(p + 24, dst6, 16);
	put16(t, 50000);
	put16(t + 2, 443);
	for (i = 0; i < plen; i++)
		t[thl + i] = (uint8_t)(i * 13 + 1);
	if (nxt == 6) {
		t[12] = 5 << 4;
		t[13] = 0x10;
	} else
		put16(t + 4, thl + plen);
	ps = rfc1071(p + 8, 32, 0) + thl + plen + nxt;
	put16(t + (nxt == 6 ? 16 : 6), ~rfc1071(t, thl + plen, ps));
	return (len);
}

/*
 * A downlink frame as the IPA writes it (rmnet's view): MAP header,
 * the packet, padding to 4 bytes, the trailer with the checksum over the
 * whole IP packet (one's complement, rmnet: "computed over the entire IP
 * packet, including the IP header and payload")
 */
static size_t
frame(uint8_t *f, uint8_t mux, const uint8_t *ip, size_t len, bool valid)
{
	size_t pad = (4 - len % 4) % 4;

	f[0] = (uint8_t)pad;
	f[1] = mux;
	put16(f + 2, len + pad);
	memcpy(f + 4, ip, len);
	memset(f + 4 + len, 0, pad);
	memset(f + 4 + len + pad, 0, 8);
	f[4 + len + pad + 1] = valid ? 1 : 0;
	put16(f + 4 + len + pad + 2, 0);
	put16(f + 4 + len + pad + 4, len);
	put16(f + 4 + len + pad + 6, ~rfc1071(ip, len, 0));
	return (4 + len + pad + 8);
}

/* --- collecting the deaggregated packets ---------------------------------------- */

static struct qmap_pkt got[16];
static uint8_t gotdata[16][2048];
static int ngot;

static void
collect(void *arg, const struct qmap_pkt *p)
{
	(void)arg;
	assert(ngot < 16);
	got[ngot] = *p;
	memcpy(gotdata[ngot], p->ip, p->len);
	got[ngot].ip = gotdata[ngot];
	ngot++;
}

static int
run(const uint8_t *buf, size_t len, struct qmap_stats *st)
{
	ngot = 0;
	memset(st, 0, sizeof(*st));
	return (qmap_deaggregate(buf, len, true, collect, NULL, st));
}

/* --- tests ---------------------------------------------------------------------------- */

static void
test_layouts(void)
{
	/* if_rmnet.h */
	assert(sizeof(struct rmnet_map_header) == QMAP_HDR_LEN);
	assert(offsetof(struct rmnet_map_header, mux_id) == 1);
	assert(offsetof(struct rmnet_map_header, pkt_len) == 2);
	assert(sizeof(struct rmnet_map_dl_csum_trailer) == QMAP_DL_TRAILER_LEN);
	assert(offsetof(struct rmnet_map_dl_csum_trailer, flags) == 1);
	assert(offsetof(struct rmnet_map_dl_csum_trailer, csum_value) == 6);
	assert(sizeof(struct rmnet_map_ul_csum_header) == QMAP_UL_HDR_LEN);
	assert(offsetof(struct rmnet_map_ul_csum_header, csum_info) == 2);
	assert(MAP_PAD_LEN_MASK == QMAP_FLAGS_PAD_MASK);
	assert(MAP_NEXT_HEADER_FLAG == QMAP_FLAGS_NEXT_HDR);
	assert(MAP_CMD_FLAG == QMAP_FLAGS_CMD);
	assert(MAP_CSUM_DL_VALID_FLAG == QMAP_DL_CSUM_VALID);
	assert(MAP_CSUM_UL_OFFSET_MASK == QMAP_UL_OFFSET_MASK);
	assert(MAP_CSUM_UL_UDP_FLAG == QMAP_UL_UDP);
	assert(MAP_CSUM_UL_ENABLED_FLAG == QMAP_UL_ENABLED);
	/* the IPA's header length on AP_MODEM_TX: MAP + UL checksum header */
	assert(QMAP_TX_HDR_LEN == sizeof(struct rmnet_map_header) +
	    sizeof(struct rmnet_map_ul_csum_header));
	/* gsi_trans.c's struct gsi_tre, gsi.c's struct gsi_event */
	assert(sizeof(struct gsi_tre) == GSI_RING_ELEMENT_SIZE);
	assert(offsetof(struct gsi_tre, len_opcode) == 8);
	assert(offsetof(struct gsi_tre, flags) == 12);
	assert(sizeof(struct gsi_event) == GSI_RING_ELEMENT_SIZE);
	assert(offsetof(struct gsi_event, len) == 8);
	assert(offsetof(struct gsi_event, code) == 11);
	assert(offsetof(struct gsi_event, type) == 14);
	assert(offsetof(struct gsi_event, chid) == 15);
	assert(TRE_FLAGS_CHAIN == GENMASK(0, 0) && TRE_FLAGS_IEOT == GENMASK(9, 9) &&
	    TRE_FLAGS_BEI == GENMASK(10, 10) && TRE_FLAGS_TYPE == GENMASK(23, 16));
	assert(sizeof(struct gsi_channel_scratch_gpi) == 16);
	assert(offsetof(struct gsi_channel_scratch_gpi, max_outstanding_tre) == 10);
	assert(offsetof(struct gsi_channel_scratch_gpi, outstanding_threshold) == 14);
	/* ipa_cmd.c's payloads */
	assert(sizeof(struct ipa_cmd_hw_ip_fltrt_init) == 24);
	assert(sizeof(struct ipa_cmd_hw_hdr_init_local) == 16);
	assert(sizeof(struct ipa_cmd_register_write) == 16);
	assert(offsetof(struct ipa_cmd_register_write, value_mask) == 8);
	assert(sizeof(struct ipa_cmd_ip_packet_init) == 8);
	assert(sizeof(struct ipa_cmd_hw_dma_mem_mem) == 16);
	assert(offsetof(struct ipa_cmd_hw_dma_mem_mem, system_addr) == 8);
	assert(sizeof(struct ipa_cmd_ip_packet_tag_status) == 8);
	assert(sizeof(union ipa_cmd_payload) == 24);
	/* ipa_cmd.c's masks */
	assert(GENMASK_ULL(55, 40) >> IP_FLTRT_FLAGS_NHASH_ADDR_SHIFT == 0xffff);
	assert(GENMASK_ULL(39, 28) >> IP_FLTRT_FLAGS_NHASH_SIZE_SHIFT == 0xfff);
	assert(GENMASK_ULL(27, 12) >> IP_FLTRT_FLAGS_HASH_ADDR_SHIFT == 0xffff);
	assert(HDR_INIT_LOCAL_FLAGS_TABLE_SIZE == GENMASK(11, 0) &&
	    HDR_INIT_LOCAL_FLAGS_HDR_ADDR == GENMASK(27, 12));
	assert(REGISTER_WRITE_OPCODE_CLEAR_OPTION == GENMASK(10, 9) &&
	    REGISTER_WRITE_FLAGS_OFFSET_HIGH == GENMASK(14, 11));
	printf("qmap: layouts as if_rmnet.h, gsi.c, gsi_trans.c, ipa_cmd.c: ok\n");
}

static void
test_downlink(void)
{
	static uint8_t buf[16384], ip[2048], ip2[2048], ip3[2048];
	struct qmap_stats st;
	size_t n = 0, l1, l2, l3;

	/* TCP over IPv4, 101 bytes of payload (3 bytes padding) */
	l1 = ipv4(ip, 6, 101, true, 0);
	n += frame(buf + n, 1, ip, l1, true);
	/* a MAP command between (dropped, counted) */
	buf[n] = QMAP_FLAGS_CMD;
	buf[n + 1] = 1;
	put16(buf + n + 2, 8);
	memset(buf + n + 4, 0xee, 8 + 8);
	n += 4 + 8 + 8;
	/* UDP over IPv4 with checksum, mux 2 */
	l2 = ipv4(ip2, 17, 30, true, 0);
	n += frame(buf + n, 2, ip2, l2, true);
	/* TCP over IPv6 */
	l3 = ipv6(ip3, 6, 1000);
	n += frame(buf + n, 1, ip3, l3, true);
	assert(run(buf, n, &st) == 3);
	assert(got[0].mux_id == 1 && got[0].len == l1 && got[0].version == 4 &&
	    memcmp(got[0].ip, ip, l1) == 0 && got[0].csum == QMAP_CSUM_OK);
	assert(got[1].mux_id == 2 && got[1].len == l2 && got[1].csum == QMAP_CSUM_OK);
	assert(got[2].version == 6 && got[2].len == l3 && got[2].csum == QMAP_CSUM_OK);
	assert(st.commands == 1 && st.packets == 3 && st.csum_ok == 3);
	printf("qmap: aggregated TCP/IPv4 (padded), a command, UDP/IPv4, "
	    "TCP/IPv6: ok\n");

	/* UDP without checksum; UDP over IPv6; ICMP; a fragment */
	n = 0;
	l1 = ipv4(ip, 17, 17, false, 0);
	n += frame(buf + n, 1, ip, l1, true);
	l2 = ipv6(ip2, 17, 40);
	n += frame(buf + n, 1, ip2, l2, true);
	l3 = ipv4(ip3, 1, 56, false, 0);
	n += frame(buf + n, 1, ip3, l3, true);
	l3 = ipv4(ip3, 6, 64, true, 0x2000);
	n += frame(buf + n, 1, ip3, l3, true);
	assert(run(buf, n, &st) == 4);
	assert(got[0].csum == QMAP_CSUM_SKIPPED);
	assert(got[1].csum == QMAP_CSUM_OK);
	assert(got[2].csum == QMAP_CSUM_NONE);
	assert(got[3].csum == QMAP_CSUM_NONE);
	printf("qmap: UDP without checksum, UDP/IPv6, ICMP, a fragment: ok\n");

	/* changed on the way: the checksum the sender made no longer fits */
	n = 0;
	l1 = ipv4(ip, 6, 200, true, 0);
	ip[60] ^= 0x40;
	n += frame(buf + n, 1, ip, l1, true);
	l2 = ipv6(ip2, 6, 200);
	ip2[100] ^= 0x01;
	n += frame(buf + n, 1, ip2, l2, true);
	/* a bad IPv4 header */
	l3 = ipv4(ip3, 6, 10, true, 0);
	ip3[8] = 63;
	n += frame(buf + n, 1, ip3, l3, true);
	/* no valid flag in the trailer */
	l3 = ipv4(ip3, 6, 10, true, 0);
	n += frame(buf + n, 3, ip3, l3, false);
	assert(run(buf, n, &st) == 4);
	assert(got[0].csum == QMAP_CSUM_BAD && got[1].csum == QMAP_CSUM_BAD);
	assert(got[2].csum == QMAP_CSUM_BAD && got[3].csum == QMAP_CSUM_NONE);
	assert(got[3].mux_id == 3);
	printf("qmap: a changed TCP/IPv4 and TCP/IPv6 payload, a bad IPv4 "
	    "header, no valid flag: ok\n");

	/* an empty frame ends the buffer (rmnet); so does a truncated one */
	n = 0;
	l1 = ipv4(ip, 6, 10, true, 0);
	n += frame(buf + n, 1, ip, l1, true);
	memset(buf + n, 0, 12);
	n += 12;
	n += frame(buf + n, 1, ip, l1, true);
	assert(run(buf, n, &st) == 1 && st.empty == 1);
	n = frame(buf, 1, ip, l1, true);
	n += frame(buf + n, 1, ip, l1, true);
	assert(run(buf, n - 5, &st) == 1 && st.truncated == 1);
	printf("qmap: an empty frame, a truncated one: ok\n");
}

static void
test_uplink(void)
{
	uint8_t h[QMAP_TX_HDR_LEN], ip[256], f[2];
	static const uint8_t tcp4[8] = { 0, 1, 0, 60, 0, 20, 0x80, 16 };
	static const uint8_t udp6[8] = { 0, 4, 0x05, 0xdc, 0, 40, 0xc0, 6 };
	static const uint8_t none[8] = { 0, 2, 0, 28, 0, 0, 0, 0 };
	uint16_t p;
	size_t len;

	/* rmnet_map_ipv4_ul_csum_header: start = the IP header, info:
	 * enabled | the offset of the field in the TCP header (16) */
	qmap_ul_header(h, 1, 60, true, false, 20, 16);
	assert(memcmp(h, tcp4, 8) == 0);
	qmap_ul_header(h, 4, 1500, true, true, 40, 6);
	assert(memcmp(h, udp6, 8) == 0);
	/* without offload the UL header is zeroed (its sw_csum path) */
	qmap_ul_header(h, 2, 28, false, false, 0, 0);
	assert(memcmp(h, none, 8) == 0);
	/* the field: the stack's pseudo-header sum (Linux's ~tcp_v4_check(),
	 * FreeBSD's in_pseudo()), complemented for the IPA */
	len = ipv4(ip, 6, 40, true, 0);
	p = rfc1071(ip + 12, 8, 0);
	p = rfc1071((const uint8_t[]){ 0, 6, (len - 20) >> 8, (len - 20) & 0xff },
	    4, p);
	put16(f, p);
	qmap_ul_complement(f);
	assert(((uint16_t)(f[0] << 8 | f[1])) == (uint16_t)~p);
	printf("qmap: the uplink headers (MAP, MAPv4 checksum), the field "
	    "complemented: ok\n");
}

int
main(void)
{
	test_layouts();
	test_downlink();
	test_uplink();
	return (0);
}
