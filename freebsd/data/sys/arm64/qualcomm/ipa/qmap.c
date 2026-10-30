/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * QMAP with MAPv4 checksum offload on byte buffers (qmap.h), after Linux's
 * rmnet: rmnet_map_deaggregate(), __rmnet_map_ingress_handler(),
 * rmnet_map_ipv4_dl_csum_trailer(), rmnet_map_ipv6_dl_csum_trailer(),
 * rmnet_map_ipv{4,6}_ul_csum_header(), rmnet_map_add_map_header() (no
 * padding, as rmnet's egress handler asks for).
 */

#include "qmap.h"

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#else
#include <string.h>
#endif

#define	IPPROTO_TCP_	6
#define	IPPROTO_UDP_	17
#define	IP_MF_		0x2000
#define	IP_OFFMASK_	0x1fff

static inline uint16_t
be16(const uint8_t *p)
{
	return ((uint16_t)(p[0] << 8 | p[1]));
}

static inline void
put_be16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v & 0xff;
}

uint32_t
qmap_sum(const uint8_t *p, size_t len, uint32_t sum)
{
	uint64_t s = sum;

	while (len > 1) {
		s += be16(p);
		p += 2;
		len -= 2;
	}
	if (len)
		s += (uint32_t)p[0] << 8;
	while (s >> 32)
		s = (s & 0xffffffffU) + (s >> 32);
	return ((uint32_t)s);
}

uint16_t
qmap_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ((uint16_t)sum);
}

/* csum16_add() of Linux: a one's complement addition of two 16-bit sums */
static uint16_t
csum16_add(uint16_t a, uint16_t b)
{
	uint32_t r = (uint32_t)a + b;

	return ((uint16_t)(r + (r >> 16)));
}

/*
 * The trailer holds the IPA's checksum over the whole IP packet; with a
 * valid IPv4 header that is the payload's. It passes when it is the
 * negated pseudo-header checksum (rmnet's comments: the two are additive
 * inverses), i.e. the folded pseudo-header sum.
 */
static enum qmap_csum
qmap_dl_csum_v4(const uint8_t *ip, uint16_t len, uint16_t trailer_csum)
{
	uint32_t ihl = (ip[0] & 0x0f) * 4, sum, tot_len;
	uint8_t proto = ip[9];
	const uint8_t *th;
	uint16_t pseudo;

	if (ihl < 20 || len < ihl)
		return (QMAP_CSUM_BAD);
	/* ip_fast_csum() over the header, its checksum included: 0 */
	if ((uint16_t)~qmap_fold(qmap_sum(ip, ihl, 0)) != 0)
		return (QMAP_CSUM_BAD);
	/* no offload on fragments */
	if (be16(ip + 6) & (IP_MF_ | IP_OFFMASK_))
		return (QMAP_CSUM_NONE);
	th = ip + ihl;
	if (proto == IPPROTO_TCP_) {
		if (len < ihl + 18)
			return (QMAP_CSUM_NONE);
	} else if (proto == IPPROTO_UDP_) {
		if (len < ihl + 8)
			return (QMAP_CSUM_NONE);
		/* RFC 768: no checksum */
		if (be16(th + 6) == 0)
			return (QMAP_CSUM_SKIPPED);
	} else
		return (QMAP_CSUM_NONE);
	tot_len = be16(ip + 2);
	if (tot_len < ihl)
		return (QMAP_CSUM_BAD);
	/* csum_tcpudp_magic(saddr, daddr, tot_len - ihl, proto, 0), negated */
	sum = qmap_sum(ip + 12, 8, 0) + proto + (tot_len - ihl);
	pseudo = qmap_fold(sum);
	return (trailer_csum == pseudo ? QMAP_CSUM_OK : QMAP_CSUM_BAD);
}

static enum qmap_csum
qmap_dl_csum_v6(const uint8_t *ip, uint16_t len, uint16_t trailer_csum)
{
	uint8_t nexthdr = ip[6];
	uint16_t hdr_csum, payload, pseudo;
	uint32_t sum;

	if (len < 40)
		return (QMAP_CSUM_BAD);
	/* TCP or UDP right after the header: no extension headers */
	if (nexthdr != IPPROTO_TCP_ && nexthdr != IPPROTO_UDP_)
		return (QMAP_CSUM_NONE);
	/* the header's share out of the trailer (csum16_sub) */
	hdr_csum = (uint16_t)~qmap_fold(qmap_sum(ip, 40, 0));
	payload = csum16_add(trailer_csum, (uint16_t)~hdr_csum);
	/* csum_ipv6_magic(saddr, daddr, payload_len, nexthdr, 0), negated */
	sum = qmap_sum(ip + 8, 32, 0) + be16(ip + 4) + nexthdr;
	pseudo = qmap_fold(sum);
	return (payload == pseudo ? QMAP_CSUM_OK : QMAP_CSUM_BAD);
}

enum qmap_csum
qmap_dl_csum(const uint8_t *ip, uint16_t len, const uint8_t *trailer)
{
	uint16_t csum;

	if ((trailer[1] & QMAP_DL_CSUM_VALID) == 0)
		return (QMAP_CSUM_NONE);
	csum = be16(trailer + 6);
	if (len < 1)
		return (QMAP_CSUM_NONE);
	switch (ip[0] >> 4) {
	case 4:
		return (qmap_dl_csum_v4(ip, len, csum));
	case 6:
		return (qmap_dl_csum_v6(ip, len, csum));
	default:
		return (QMAP_CSUM_NONE);
	}
}

int
qmap_deaggregate(const uint8_t *buf, uint32_t len, bool csum_v4,
    void (*cb)(void *, const struct qmap_pkt *), void *arg,
    struct qmap_stats *st)
{
	struct qmap_pkt pkt;
	uint32_t off = 0, frame, pkt_len, pad;
	uint8_t flags;
	int n = 0;

	while (off < len) {
		if (len - off < QMAP_HDR_LEN) {
			st->truncated++;
			break;
		}
		flags = buf[off];
		pkt_len = be16(buf + off + 2);
		frame = QMAP_HDR_LEN + pkt_len;
		if (csum_v4)
			frame += QMAP_DL_TRAILER_LEN;
		if (frame > len - off) {
			st->truncated++;
			break;
		}
		/* some hardware sends empty frames */
		if (pkt_len == 0) {
			st->empty++;
			break;
		}
		if (flags & QMAP_FLAGS_CMD) {
			st->commands++;
			off += frame;
			continue;
		}
		pad = flags & QMAP_FLAGS_PAD_MASK;
		if (pad > pkt_len || buf[off + 1] > QMAP_MUX_MAX) {
			st->bad_mux += buf[off + 1] > QMAP_MUX_MAX;
			off += frame;
			continue;
		}
		memset(&pkt, 0, sizeof(pkt));
		pkt.mux_id = buf[off + 1];
		pkt.ip = buf + off + QMAP_HDR_LEN;
		pkt.len = (uint16_t)(pkt_len - pad);
		pkt.version = pkt.len > 0 ? pkt.ip[0] >> 4 : 0;
		if (pkt.version != 4 && pkt.version != 6)
			pkt.version = 0;
		/* the trailer follows the padding */
		pkt.csum = csum_v4 ? qmap_dl_csum(pkt.ip, pkt.len, pkt.ip +
		    pkt_len) : QMAP_CSUM_NONE;
		switch (pkt.csum) {
		case QMAP_CSUM_OK: st->csum_ok++; break;
		case QMAP_CSUM_SKIPPED: st->csum_skipped++; break;
		case QMAP_CSUM_NONE: st->csum_none++; break;
		case QMAP_CSUM_BAD: st->csum_bad++; break;
		}
		st->packets++;
		n++;
		cb(arg, &pkt);
		off += frame;
	}
	return (n);
}

void
qmap_ul_header(uint8_t hdr[QMAP_TX_HDR_LEN], uint8_t mux_id, uint16_t ip_len,
    bool csum, bool udp, uint16_t csum_start, uint16_t csum_offset)
{
	uint16_t info;

	/* a data packet, no padding: pkt_len is the IP packet's length */
	hdr[0] = 0;
	hdr[1] = mux_id;
	put_be16(hdr + 2, ip_len);
	if (!csum) {
		memset(hdr + QMAP_HDR_LEN, 0, QMAP_UL_HDR_LEN);
		return;
	}
	info = QMAP_UL_ENABLED | (csum_offset & QMAP_UL_OFFSET_MASK);
	if (udp)
		info |= QMAP_UL_UDP;
	put_be16(hdr + 4, csum_start);
	put_be16(hdr + 6, info);
}

/* rmnet_map_complement_ipv{4,6}_txporthdr_csum_field() */
void
qmap_ul_complement(uint8_t *field)
{
	put_be16(field, (uint16_t)~be16(field));
}
