/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * QMAP, the modem's multiplexing and aggregation protocol, with MAPv4
 * checksum offload, as Linux's rmnet does it for the IPA (drivers/net/
 * ethernet/qualcomm/rmnet: rmnet_map_data.c, rmnet_handlers.c; the formats
 * in include/linux/if_rmnet.h). On byte buffers, for the kernel and the
 * tests on the PC alike.
 *
 * Downlink (the IPA's AP_MODEM_RX buffers, aggregated): per packet
 *   MAP header (4) | IP packet | padding (MAP_PAD_LEN) | DL csum trailer (8)
 * Uplink (AP_MODEM_TX, one packet per transfer):
 *   MAP header (4) | UL csum header (4) | IP packet
 * the UL header zeroed when no checksum is asked of the IPA.
 */

#ifndef _QMAP_H_
#define	_QMAP_H_

#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#endif

/* struct rmnet_map_header */
#define	QMAP_HDR_LEN		4
#define	QMAP_FLAGS_PAD_MASK	0x3f	/* MAP_PAD_LEN_MASK */
#define	QMAP_FLAGS_NEXT_HDR	0x40	/* MAP_NEXT_HEADER_FLAG (MAPv5) */
#define	QMAP_FLAGS_CMD		0x80	/* MAP_CMD_FLAG */
/* struct rmnet_map_dl_csum_trailer: reserved1, flags, csum_start_offset,
 * csum_length, csum_value (each 16-bit field big endian) */
#define	QMAP_DL_TRAILER_LEN	8
#define	QMAP_DL_CSUM_VALID	0x01	/* MAP_CSUM_DL_VALID_FLAG, in flags */
/* struct rmnet_map_ul_csum_header: csum_start_offset, csum_info */
#define	QMAP_UL_HDR_LEN		4
#define	QMAP_UL_OFFSET_MASK	0x3fff	/* MAP_CSUM_UL_OFFSET_MASK */
#define	QMAP_UL_UDP		0x4000	/* MAP_CSUM_UL_UDP_FLAG */
#define	QMAP_UL_ENABLED		0x8000	/* MAP_CSUM_UL_ENABLED_FLAG */
#define	QMAP_TX_HDR_LEN		(QMAP_HDR_LEN + QMAP_UL_HDR_LEN)
#define	QMAP_MUX_MAX		254	/* rmnet: mux ids below 255 */

/* what the DL checksum trailer says (rmnet_map_checksum_downlink_packet) */
enum qmap_csum {
	QMAP_CSUM_OK,		/* the transport checksum is right */
	QMAP_CSUM_SKIPPED,	/* UDP over IPv4 without checksum: fine too */
	QMAP_CSUM_NONE,		/* not checked (trailer not valid, other
				 * protocol, fragment): the stack checks */
	QMAP_CSUM_BAD,		/* wrong (IPv4 header or transport) */
};

struct qmap_pkt {
	const uint8_t	*ip;		/* the IP packet */
	uint16_t	len;		/* its length (without padding) */
	uint8_t		mux_id;
	uint8_t		version;	/* 4, 6, else 0 */
	enum qmap_csum	csum;
};

struct qmap_stats {
	uint64_t	packets, commands, empty, truncated, bad_mux;
	uint64_t	csum_ok, csum_skipped, csum_none, csum_bad;
};

/*
 * The packets of an aggregated buffer (rmnet_map_deaggregate and the ingress
 * handler, flags INGRESS_DEAGGREGATION | INGRESS_MAP_CKSUMV4 when csum_v4);
 * as rmnet, the rest of the buffer goes when a frame is empty or runs past
 * its end. Commands (MAP_CMD_FLAG) are counted and dropped (no
 * INGRESS_MAP_COMMANDS). The number of packets given to cb.
 */
int	qmap_deaggregate(const uint8_t *buf, uint32_t len, bool csum_v4,
	    void (*cb)(void *, const struct qmap_pkt *), void *arg,
	    struct qmap_stats *st);

/* The DL trailer of a packet (ip, len) checked as rmnet does */
enum qmap_csum qmap_dl_csum(const uint8_t *ip, uint16_t len,
	    const uint8_t *trailer);

/*
 * The uplink headers for an IP packet of ip_len bytes on mux_id: hdr gets
 * the MAP header and the UL checksum header. With csum (the transport
 * checksum asked of the IPA: rmnet_map_ipv4/ipv6_ul_csum_header) the
 * transport header starts csum_start bytes into the packet and its
 * checksum field is csum_offset bytes into that; the caller then
 * complements that field (qmap_ul_complement), which must hold the
 * pseudo-header's sum as the stack's offload leaves it.
 */
void	qmap_ul_header(uint8_t hdr[QMAP_TX_HDR_LEN], uint8_t mux_id,
	    uint16_t ip_len, bool csum, bool udp, uint16_t csum_start,
	    uint16_t csum_offset);
void	qmap_ul_complement(uint8_t *field);

/* RFC 1071 helpers: the 16-bit one's complement sum, big-endian words */
uint32_t qmap_sum(const uint8_t *p, size_t len, uint32_t sum);
uint16_t qmap_fold(uint32_t sum);

#endif /* _QMAP_H_ */
