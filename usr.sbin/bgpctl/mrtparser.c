/*	$OpenBSD: mrtparser.c,v 1.21 2024/01/23 16:16:15 claudio Exp $ */
/*
 * Copyright (c) 2011 Claudio Jeker <claudio@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mrt.h"
#include "mrtparser.h"

void	*mrt_read_msg(int, struct mrt_hdr *);
size_t	 mrt_read_buf(int, void *, size_t);

struct mrt_peer	*mrt_parse_v2_peer(struct mrt_hdr *, void *);
struct mrt_rib	*mrt_parse_v2_rib(struct mrt_hdr *, void *, int);
int	mrt_parse_dump(struct mrt_hdr *, void *, struct mrt_peer **,
	    struct mrt_rib **);
int	mrt_parse_dump_mp(struct mrt_hdr *, void *, struct mrt_peer **,
	    struct mrt_rib **, int);
int	mrt_extract_attr(struct mrt_rib_entry *, u_char *, int, uint8_t, int);

void	mrt_free_peers(struct mrt_peer *);
void	mrt_free_rib(struct mrt_rib *);
void	mrt_free_bgp_state(struct mrt_bgp_state *);
void	mrt_free_bgp_msg(struct mrt_bgp_msg *);

u_char *mrt_aspath_inflate(void *, uint16_t, uint16_t *);
int	mrt_extract_addr(void *, u_int, struct bgpd_addr *, uint8_t);
int	mrt_extract_prefix(void *, u_int, uint8_t, struct bgpd_addr *,
	    uint8_t *, int);

struct mrt_bgp_state	*mrt_parse_state(struct mrt_hdr *, void *, int);
struct mrt_bgp_msg	*mrt_parse_msg(struct mrt_hdr *, void *, int);

void *
mrt_read_msg(int fd, struct mrt_hdr *hdr)
{
	void *buf;

	memset(hdr, 0, sizeof(*hdr));
	if (mrt_read_buf(fd, hdr, sizeof(*hdr)) != sizeof(*hdr))
		return (NULL);

	if ((buf = malloc(ntohl(hdr->length))) == NULL)
		err(1, "malloc(%d)", hdr->length);

	if (mrt_read_buf(fd, buf, ntohl(hdr->length)) != ntohl(hdr->length)) {
		free(buf);
		return (NULL);
	}
	return (buf);
}

size_t
mrt_read_buf(int fd, void *buf, size_t len)
{
	char *b = buf;
	ssize_t n;

	while (len > 0) {
		if ((n = read(fd, b, len)) == -1) {
			if (errno == EINTR)
				continue;
			err(1, "read");
		}
		if (n == 0)
			break;
		b += n;
		len -= n;
	}

	return (b - (char *)buf);
}

void
mrt_parse(int fd, struct mrt_parser *p, int verbose)
{
	struct mrt_hdr		h;
	struct mrt_peer		*pctx = NULL;
	struct mrt_rib		*r;
	struct mrt_bgp_state	*s;
	struct mrt_bgp_msg	*m;
	void			*msg;

	while ((msg = mrt_read_msg(fd, &h))) {
		switch (ntohs(h.type)) {
		case MSG_NULL:
		case MSG_START:
		case MSG_DIE:
		case MSG_I_AM_DEAD:
		case MSG_PEER_DOWN:
		case MSG_PROTOCOL_BGP:
		case MSG_PROTOCOL_IDRP:
		case MSG_PROTOCOL_BGP4PLUS:
		case MSG_PROTOCOL_BGP4PLUS1:
			if (verbose)
				printf("deprecated MRT type %d\n",
				    ntohs(h.type));
			break;
		case MSG_PROTOCOL_RIP:
		case MSG_PROTOCOL_RIPNG:
		case MSG_PROTOCOL_OSPF:
		case MSG_PROTOCOL_ISIS_ET:
		case MSG_PROTOCOL_ISIS:
		case MSG_PROTOCOL_OSPFV3_ET:
		case MSG_PROTOCOL_OSPFV3:
			if (verbose)
				printf("unsupported MRT type %d\n",
				    ntohs(h.type));
			break;
		case MSG_TABLE_DUMP:
			switch (ntohs(h.subtype)) {
			case MRT_DUMP_AFI_IP:
			case MRT_DUMP_AFI_IPv6:
				if (p->dump == NULL)
					break;
				if (mrt_parse_dump(&h, msg, &pctx, &r) == 0) {
					if (p->dump)
						p->dump(r, pctx, p->arg);
					mrt_free_rib(r);
				}
				break;
			default:
				if (verbose)
					printf("unknown AFI %d in table dump\n",
					    ntohs(h.subtype));
				break;
			}
			break;
		case MSG_TABLE_DUMP_V2:
			switch (ntohs(h.subtype)) {
			case MRT_DUMP_V2_PEER_INDEX_TABLE:
				if (p->dump == NULL)
					break;
				if (pctx)
					mrt_free_peers(pctx);
				pctx = mrt_parse_v2_peer(&h, msg);
				break;
			case MRT_DUMP_V2_RIB_IPV4_UNICAST:
			case MRT_DUMP_V2_RIB_IPV4_MULTICAST:
			case MRT_DUMP_V2_RIB_IPV6_UNICAST:
			case MRT_DUMP_V2_RIB_IPV6_MULTICAST:
			case MRT_DUMP_V2_RIB_GENERIC:
			case MRT_DUMP_V2_RIB_IPV4_UNICAST_ADDPATH:
			case MRT_DUMP_V2_RIB_IPV4_MULTICAST_ADDPATH:
			case MRT_DUMP_V2_RIB_IPV6_UNICAST_ADDPATH:
			case MRT_DUMP_V2_RIB_IPV6_MULTICAST_ADDPATH:
			case MRT_DUMP_V2_RIB_GENERIC_ADDPATH:
				if (p->dump == NULL)
					break;
				r = mrt_parse_v2_rib(&h, msg, verbose);
				if (r) {
					if (p->dump)
						p->dump(r, pctx, p->arg);
					mrt_free_rib(r);
				}
				break;
			default:
				if (verbose)
					printf("unhandled DUMP_V2 subtype %d\n",
					    ntohs(h.subtype));
				break;
			}
			break;
		case MSG_PROTOCOL_BGP4MP_ET:
		case MSG_PROTOCOL_BGP4MP:
			switch (ntohs(h.subtype)) {
			case BGP4MP_STATE_CHANGE:
			case BGP4MP_STATE_CHANGE_AS4:
				if ((s = mrt_parse_state(&h, msg, verbose))) {
					if (p->state)
						p->state(s, p->arg);
					free(s);
				}
				break;
			case BGP4MP_MESSAGE:
			case BGP4MP_MESSAGE_AS4:
			case BGP4MP_MESSAGE_LOCAL:
			case BGP4MP_MESSAGE_AS4_LOCAL:
			case BGP4MP_MESSAGE_ADDPATH:
			case BGP4MP_MESSAGE_AS4_ADDPATH:
			case BGP4MP_MESSAGE_LOCAL_ADDPATH:
			case BGP4MP_MESSAGE_AS4_LOCAL_ADDPATH:
				if ((m = mrt_parse_msg(&h, msg, verbose))) {
					if (p->message)
						p->message(m, p->arg);
					free(m->msg);
					free(m);
				}
				break;
			case BGP4MP_ENTRY:
				if (p->dump == NULL)
					break;
				if (mrt_parse_dump_mp(&h, msg, &pctx, &r,
				    verbose) == 0) {
					if (p->dump)
						p->dump(r, pctx, p->arg);
					mrt_free_rib(r);
				}
				break;
			default:
				if (verbose)
					printf("unhandled BGP4MP subtype %d\n",
					    ntohs(h.subtype));
				break;
			}
			break;
		default:
			if (verbose)
				printf("unknown MRT type %d\n", ntohs(h.type));
			break;
		}
		free(msg);
	}
	if (pctx)
		mrt_free_peers(pctx);
}

static int
mrt_afi2aid(int afi, int safi, int verbose)
{
	switch (afi) {
	case MRT_DUMP_AFI_IP:
		if (safi == -1 || safi == 1 || safi == 2)
			return AID_INET;
		else if (safi == 128)
			return AID_VPN_IPv4;
		break;
	case MRT_DUMP_AFI_IPv6:
		if (safi == -1 || safi == 1 || safi == 2)
			return AID_INET6;
		else if (safi == 128)
			return AID_VPN_IPv6;
		break;
	default:
		break;
	}
	if (verbose)
		printf("unhandled AFI/SAFI %d/%d\n", afi, safi);
	return AID_UNSPEC;
}

struct mrt_peer *
mrt_parse_v2_peer(struct mrt_hdr *hdr, void *msg)
{
	struct mrt_peer_entry	*peers = NULL;
	struct mrt_peer	*p;
	uint8_t		*b = msg;
	uint32_t	bid, as4;
	uint16_t	cnt, i, as2;
	u_int		len = ntohl(hdr->length);

	if (len < 8)	/* min msg size */
		return NULL;

	p = calloc(1, sizeof(struct mrt_peer));
	if (p == NULL)
		err(1, "calloc");

	/* collector bgp id */
	memcpy(&bid, b, sizeof(bid));
	b += sizeof(bid);
	len -= sizeof(bid);
	p->bgp_id = ntohl(bid);

	/* view name length */
	memcpy(&cnt, b, sizeof(cnt));
	b += sizeof(cnt);
	len -= sizeof(cnt);
	cnt = ntohs(cnt);

	/* view name */
	if (cnt > len)
		goto fail;
	if (cnt != 0) {
		if ((p->view = malloc(cnt + 1)) == NULL)
			err(1, "malloc");
		memcpy(p->view, b, cnt);
		p->view[cnt] = 0;
	} else
		if ((p->view = strdup("")) == NULL)
			err(1, "strdup");
	b += cnt;
	len -= cnt;

	/* peer_count */
	if (len < sizeof(cnt))
		goto fail;
	memcpy(&cnt, b, sizeof(cnt));
	b += sizeof(cnt);
	len -= sizeof(cnt);
	cnt = ntohs(cnt);

	/* peer entries */
	if ((peers = calloc(cnt, sizeof(struct mrt_peer_entry))) == NULL)
		err(1, "calloc");
	for (i = 0; i < cnt; i++) {
		uint8_t type;

		if (len < sizeof(uint8_t) + sizeof(uint32_t))
			goto fail;
		type = *b++;
		len -= 1;
		memcpy(&bid, b, sizeof(bid));
		b += sizeof(bid);
		len -= sizeof(bid);
		peers[i].bgp_id = ntohl(bid);

		if (type & MRT_DUMP_V2_PEER_BIT_I) {
			if (mrt_extract_addr(b, len, &peers[i].addr,
			    AID_INET6) == -1)
				goto fail;
			b += sizeof(struct in6_addr);
			len -= sizeof(struct in6_addr);
		} else {
			if (mrt_extract_addr(b, len, &peers[i].addr,
			    AID_INET) == -1)
				goto fail;
			b += sizeof(struct in_addr);
			len -= sizeof(struct in_addr);
		}

		if (type & MRT_DUMP_V2_PEER_BIT_A) {
			memcpy(&as4, b, sizeof(as4));
			b += sizeof(as4);
			len -= sizeof(as4);
			as4 = ntohl(as4);
		} else {
			memcpy(&as2, b, sizeof(as2));
			b += sizeof(as2);
			len -= sizeof(as2);
			as4 = ntohs(as2);
		}
		peers[i].asnum = as4;
	}
	p->peers = peers;
	p->npeers = cnt;
	return (p);
fail:
	mrt_free_peers(p);
	free(peers);
	return (NULL);
}

struct mrt_rib *
mrt_parse_v2_rib(struct mrt_hdr *hdr, void *msg, int verbose)
{
	struct mrt_rib_entry *entries = NULL;
	struct mrt_rib	*r;
	uint8_t		*b = msg;
	u_int		len = ntohl(hdr->length);
	uint32_t	snum, path_id = 0;
	uint16_t	cnt, i, afi;
	uint8_t		safi, aid;
	int		ret;

	if (len < sizeof(snum) + 1)
		return NULL;

	r = calloc(1, sizeof(struct mrt_rib));
	if (r == NULL)
		err(1, "calloc");

	/* seq_num */
	memcpy(&snum, b, sizeof(snum));
	b += sizeof(snum);
	len -= sizeof(snum);
	r->seqnum = ntohl(snum);

	switch (ntohs(hdr->subtype)) {
	case MRT_DUMP_V2_RIB_IPV4_UNICAST_ADDPATH:
	case MRT_DUMP_V2_RIB_IPV4_MULTICAST_ADDPATH:
		r->add_path = 1;
		/* FALLTHROUGH */
	case MRT_DUMP_V2_RIB_IPV4_UNICAST:
	case MRT_DUMP_V2_RIB_IPV4_MULTICAST:
		/* prefix */
		ret = mrt_extract_prefix(b, len, AID_INET, &r->prefix,
		    &r->prefixlen, verbose);
		if (ret == -1)
			goto fail;
		break;
	case MRT_DUMP_V2_RIB_IPV6_UNICAST_ADDPATH:
	case MRT_DUMP_V2_RIB_IPV6_MULTICAST_ADDPATH:
		r->add_path = 1;
		/* FALLTHROUGH */
	case MRT_DUMP_V2_RIB_IPV6_UNICAST:
	case MRT_DUMP_V2_RIB_IPV6_MULTICAST:
		/* prefix */
		ret = mrt_extract_prefix(b, len, AID_INET6, &r->prefix,
		    &r->prefixlen, verbose);
		if (ret == -1)
			goto fail;
		break;
	case MRT_DUMP_V2_RIB_GENERIC_ADDPATH:
		/*
		 * RFC8050 handling for add-path has special handling for
		 * RIB_GENERIC_ADDPATH but nobody implements it that way.
		 * So just use the same way as for the other _ADDPATH types.
		 */
		r->add_path = 1;
		/* FALLTHROUGH */
	case MRT_DUMP_V2_RIB_GENERIC:
		/* fetch AFI/SAFI pair */
		if (len < 3)
			goto fail;
		memcpy(&afi, b, sizeof(afi));
		b += sizeof(afi);
		len -= sizeof(afi);
		afi = ntohs(afi);

		safi = *b++;
		len -= 1;

		if ((aid = mrt_afi2aid(afi, safi, verbose)) == AID_UNSPEC)
			goto fail;

		/* prefix */
		ret = mrt_extract_prefix(b, len, aid, &r->prefix,
		    &r->prefixlen, verbose);
		if (ret == -1)
			goto fail;
		break;
	default:
		errx(1, "unknown subtype %hd", ntohs(hdr->subtype));
	}

	/* adjust length */
	b += ret;
	len -= ret;

	/* entries count */
	if (len < sizeof(cnt))
		goto fail;
	memcpy(&cnt, b, sizeof(cnt));
	b += sizeof(cnt);
	len -= sizeof(cnt);
	cnt = ntohs(cnt);
	r->nentries = cnt;

	/* entries */
	if ((entries = calloc(cnt, sizeof(struct mrt_rib_entry))) == NULL)
		err(1, "calloc");
	for (i = 0; i < cnt; i++) {
		uint32_t	otm;
		uint16_t	pix, alen;
		if (len < 2 * sizeof(uint16_t) + sizeof(uint32_t))
			goto fail;
		/* peer index */
		memcpy(&pix, b, sizeof(pix));
		b += sizeof(pix);
		len -= sizeof(pix);
		entries[i].peer_idx = ntohs(pix);

		/* originated */
		memcpy(&otm, b, sizeof(otm));
		b += sizeof(otm);
		len -= sizeof(otm);
		entries[i].originated = ntohl(otm);

		if (r->add_path) {
			if (len < sizeof(path_id) + sizeof(alen))
				goto fail;
			memcpy(&path_id, b, sizeof(path_id));
			b += sizeof(path_id);
			len -= sizeof(path_id);
			path_id = ntohl(path_id);
		}
		entries[i].path_id = path_id;

		/* attr_len */
		memcpy(&alen, b, sizeof(alen));
		b += sizeof(alen);
		len -= sizeof(alen);
		alen = ntohs(alen);

		/* attr */
		if (len < alen)
			goto fail;
		if (mrt_extract_attr(&entries[i], b, alen,
		    r->prefix.aid, 1) == -1)
			goto fail;
		b += alen;
		len -= alen;
	}
	r->entries = entries;
	return (r);
fail:
	mrt_free_rib(r);
	free(entries);
	return (NULL);
}

int
mrt_parse_dump(struct mrt_hdr *hdr, void *msg, struct mrt_peer **pp,
    struct mrt_rib **rp)
{
	struct mrt_peer		*p;
	struct mrt_rib		*r;
	struct mrt_rib_entry	*re;
	uint8_t			*b = msg;
	u_int			 len = ntohl(hdr->length);
	uint16_t		 asnum, alen;

	if (*pp == NULL) {
		*pp = calloc(1, sizeof(struct mrt_peer));
		if (*pp == NULL)
			err(1, "calloc");
		(*pp)->peers = calloc(1, sizeof(struct mrt_peer_entry));
		if ((*pp)->peers == NULL)
			err(1, "calloc");
		(*pp)->npeers = 1;
	}
	p = *pp;

	*rp = r = calloc(1, sizeof(struct mrt_rib));
	if (r == NULL)
		err(1, "calloc");
	re = calloc(1, sizeof(struct mrt_rib_entry));
	if (re == NULL)
		err(1, "calloc");
	r->nentries = 1;
	r->entries = re;

	if (len < 2 * sizeof(uint16_t))
		goto fail;
	/* view */
	b += sizeof(uint16_t);
	len -= sizeof(uint16_t);
	/* seqnum */
	memcpy(&r->seqnum, b, sizeof(uint16_t));
	b += sizeof(uint16_t);
	len -= sizeof(uint16_t);
	r->seqnum = ntohs(r->seqnum);

	switch (ntohs(hdr->subtype)) {
	case MRT_DUMP_AFI_IP:
		if (mrt_extract_addr(b, len, &r->prefix, AID_INET) == -1)
			goto fail;
		b += sizeof(struct in_addr);
		len -= sizeof(struct in_addr);
		break;
	case MRT_DUMP_AFI_IPv6:
		if (mrt_extract_addr(b, len, &r->prefix, AID_INET6) == -1)
			goto fail;
		b += sizeof(struct in6_addr);
		len -= sizeof(struct in6_addr);
		break;
	}
	if (len < 2 * sizeof(uint32_t) + 2 * sizeof(uint16_t) + 2)
		goto fail;
	r->prefixlen = *b++;
	len -= 1;
	/* status */
	b += 1;
	len -= 1;
	/* originated */
	memcpy(&re->originated, b, sizeof(uint32_t));
	b += sizeof(uint32_t);
	len -= sizeof(uint32_t);
	re->originated = ntohl(re->originated);
	/* peer ip */
	switch (ntohs(hdr->subtype)) {
	case MRT_DUMP_AFI_IP:
		if (mrt_extract_addr(b, len, &p->peers->addr, AID_INET) == -1)
			goto fail;
		b += sizeof(struct in_addr);
		len -= sizeof(struct in_addr);
		break;
	case MRT_DUMP_AFI_IPv6:
		if (mrt_extract_addr(b, len, &p->peers->addr, AID_INET6) == -1)
			goto fail;
		b += sizeof(struct in6_addr);
		len -= sizeof(struct in6_addr);
		break;
	}
	memcpy(&asnum, b, sizeof(asnum));
	b += sizeof(asnum);
	len -= sizeof(asnum);
	p->peers->asnum = ntohs(asnum);

	memcpy(&alen, b, sizeof(alen));
	b += sizeof(alen);
	len -= sizeof(alen);
	alen = ntohs(alen);

	/* attr */
	if (len < alen)
		goto fail;
	if (mrt_extract_attr(re, b, alen, r->prefix.aid, 0) == -1)
		goto fail;
	b += alen;
	len -= alen;

	return (0);
fail:
	mrt_free_rib(r);
	return (-1);
}

int
mrt_parse_dump_mp(struct mrt_hdr *hdr, void *msg, struct mrt_peer **pp,
    struct mrt_rib **rp, int verbose)
{
	struct mrt_peer		*p;
	struct mrt_rib		*r;
	struct mrt_rib_entry	*re;
	uint8_t			*b = msg;
	u_int			 len = ntohl(hdr->length);
	uint16_t		 asnum, alen, afi;
	uint8_t			 safi, nhlen, aid;
	int			 ret;

	/* just ignore the microsec field for _ET header for now */
	if (ntohs(hdr->type) == MSG_PROTOCOL_BGP4MP_ET) {
		b = (char *)b + sizeof(uint32_t);
		len -= sizeof(uint32_t);
	}

	if (*pp == NULL) {
		*pp = calloc(1, sizeof(struct mrt_peer));
		if (*pp == NULL)
			err(1, "calloc");
		(*pp)->peers = calloc(1, sizeof(struct mrt_peer_entry));
		if ((*pp)->peers == NULL)
			err(1, "calloc");
		(*pp)->npeers = 1;
	}
	p = *pp;

	*rp = r = calloc(1, sizeof(struct mrt_rib));
	if (r == NULL)
		err(1, "calloc");
	re = calloc(1, sizeof(struct mrt_rib_entry));
	if (re == NULL)
		err(1, "calloc");
	r->nentries = 1;
	r->entries = re;

	if (len < 4 * sizeof(uint16_t))
		goto fail;
	/* source AS */
	b += sizeof(uint16_t);
	len -= sizeof(uint16_t);
	/* dest AS */
	memcpy(&asnum, b, sizeof(asnum));
	b += sizeof(asnum);
	len -= sizeof(asnum);
	p->peers->asnum = ntohs(asnum);
	/* iface index */
	b += sizeof(uint16_t);
	len -= sizeof(uint16_t);
	/* afi */
	memcpy(&afi, b, sizeof(afi));
	b += sizeof(afi);
	len -= sizeof(afi);
	afi = ntohs(afi);

	/* source + dest ip */
	switch (afi) {
	case MRT_DUMP_AFI_IP:
		if (len < 2 * sizeof(struct in_addr))
			goto fail;
		/* source IP */
		b += sizeof(struct in_addr);
		len -= sizeof(struct in_addr);
		/* dest IP */
		if (mrt_extract_addr(b, len, &p->peers->addr, AID_INET) == -1)
			goto fail;
		b += sizeof(struct in_addr);
		len -= sizeof(struct in_addr);
		break;
	case MRT_DUMP_AFI_IPv6:
		if (len < 2 * sizeof(struct in6_addr))
			goto fail;
		/* source IP */
		b += sizeof(struct in6_addr);
		len -= sizeof(struct in6_addr);
		/* dest IP */
		if (mrt_extract_addr(b, len, &p->peers->addr, AID_INET6) == -1)
			goto fail;
		b += sizeof(struct in6_addr);
		len -= sizeof(struct in6_addr);
		break;
	}

	if (len < 2 * sizeof(uint16_t) + 2 * sizeof(uint32_t))
		goto fail;
	/* view + status */
	b += 2 * sizeof(uint16_t);
	len -= 2 * sizeof(uint16_t);
	/* originated */
	memcpy(&re->originated, b, sizeof(uint32_t));
	b += sizeof(uint32_t);
	len -= sizeof(uint32_t);
	re->originated = ntohl(re->originated);

	/* afi */
	memcpy(&afi, b, sizeof(afi));
	b += sizeof(afi);
	len -= sizeof(afi);
	afi = ntohs(afi);

	/* safi */
	safi = *b++;
	len -= 1;

	if ((aid = mrt_afi2aid(afi, safi, verbose)) == AID_UNSPEC)
		goto fail;

	/* nhlen */
	nhlen = *b++;
	len -= 1;

	/* nexthop */
	if (mrt_extract_addr(b, len, &re->nexthop, aid) == -1)
		goto fail;
	if (len < nhlen)
		goto fail;
	b += nhlen;
	len -= nhlen;

	/* prefix */
	ret = mrt_extract_prefix(b, len, aid, &r->prefix, &r->prefixlen,
	    verbose);
	if (ret == -1)
		goto fail;
	b += ret;
	len -= ret;

	memcpy(&alen, b, sizeof(alen));
	b += sizeof(alen);
	len -= sizeof(alen);
	alen = ntohs(alen);

	/* attr */
	if (len < alen)
		goto fail;
	if (mrt_extract_attr(re, b, alen, r->prefix.aid, 0) == -1)
		goto fail;
	b += alen;
	len -= alen;

	return (0);
fail:
	mrt_free_rib(r);
	return (-1);
}

int
mrt_extract_attr(struct mrt_rib_entry *re, u_char *a, int alen, uint8_t aid,
    int as4)
{
	struct mrt_attr	*ap;
	uint32_t	tmp;
	uint16_t	attr_len;
	uint8_t		type, flags, *attr;

	do {
		if (alen < 3)
			return (-1);
		attr = a;
		flags = *a++;
		alen -= 1;
		type = *a++;
		alen -= 1;

		if (flags & MRT_ATTR_EXTLEN) {
			if (alen < 2)
				return (-1);
			memcpy(&attr_len, a, sizeof(attr_len));
			attr_len = ntohs(attr_len);
			a += sizeof(attr_len);
			alen -= sizeof(attr_len);
		} else {
			attr_len = *a++;
			alen -= 1;
		}
		switch (type) {
		case MRT_ATTR_ORIGIN:
			if (attr_len != 1)
				return (-1);
			re->origin = *a;
			break;
		case MRT_ATTR_ASPATH:
			if (as4) {
				re->aspath_len = attr_len;
				if ((re->aspath = malloc(attr_len)) == NULL)
					err(1, "malloc");
				memcpy(re->aspath, a, attr_len);
			} else {
				re->aspath = mrt_aspath_inflate(a, attr_len,
				    &re->aspath_len);
				if (re->aspath == NULL)
					return (-1);
			}
			break;
		case MRT_ATTR_NEXTHOP:
			if (attr_len != 4)
				return (-1);
			if (aid != AID_INET)
				break;
			memcpy(&tmp, a, sizeof(tmp));
			re->nexthop.aid = AID_INET;
			re->nexthop.v4.s_addr = tmp;
			break;
		case MRT_ATTR_MED:
			if (attr_len != 4)
				return (-1);
			memcpy(&tmp, a, sizeof(tmp));
			re->med = ntohl(tmp);
			break;
		case MRT_ATTR_LOCALPREF:
			if (attr_len != 4)
				return (-1);
			memcpy(&tmp, a, sizeof(tmp));
			re->local_pref = ntohl(tmp);
			break;
		case MRT_ATTR_MP_REACH_NLRI:
			/*
			 * XXX horrible hack:
			 * Once again IETF and the real world differ in the
			 * implementation. In short the abbreviated MP_NLRI
			 * hack in the standard is not used in real life.
			 * Detect the two cases by looking at the first byte
			 * of the payload (either the nexthop addr length (RFC)
			 * or the high byte of the AFI (old form)). If the
			 * first byte matches the expected nexthop length it
			 * is expected to be the RFC 6396 encoding.
			 */
			if (*a != attr_len - 1) {
				a += 3;
				alen -= 3;
				attr_len -= 3;
			}
			switch (aid) {
			case AID_INET6:
				if (attr_len < sizeof(struct in6_addr) + 1)
					return (-1);
				re->nexthop.aid = aid;
				memcpy(&re->nexthop.v6, a + 1,
				    sizeof(struct in6_addr));
				break;
			case AID_VPN_IPv4:
				if (attr_len < sizeof(uint64_t) +
				    sizeof(struct in_addr))
					return (-1);
				re->nexthop.aid = aid;
				memcpy(&tmp, a + 1 + sizeof(uint64_t),
				    sizeof(tmp));
				re->nexthop.v4.s_addr = tmp;
				break;
			case AID_VPN_IPv6:
				if (attr_len < sizeof(uint64_t) +
				    sizeof(struct in6_addr))
					return (-1);
				re->nexthop.aid = aid;
				memcpy(&re->nexthop.v6,
				    a + 1 + sizeof(uint64_t),
				    sizeof(struct in6_addr));
				break;
			}
			break;
		case MRT_ATTR_AS4PATH:
			if (!as4) {
				free(re->aspath);
				re->aspath_len = attr_len;
				if ((re->aspath = malloc(attr_len)) == NULL)
					err(1, "malloc");
				memcpy(re->aspath, a, attr_len);
				break;
			}
			/* FALLTHROUGH */
		default:
			re->nattrs++;
			if (re->nattrs >= UCHAR_MAX)
				err(1, "too many attributes");
			ap = reallocarray(re->attrs,
			    re->nattrs, sizeof(struct mrt_attr));
			if (ap == NULL)
				err(1, "realloc");
			re->attrs = ap;
			ap = re->attrs + re->nattrs - 1;
			ap->attr_len = a + attr_len - attr;
			if ((ap->attr = malloc(ap->attr_len)) == NULL)
				err(1, "malloc");
			memcpy(ap->attr, attr, ap->attr_len);
			break;
		}
		a += attr_len;
		alen -= attr_len;
	} while (alen > 0);

	return (0);
}

void
mrt_free_peers(struct mrt_peer *p)
{
	free(p->peers);
	free(p->view);
	free(p);
}

void
mrt_free_rib(struct mrt_rib *r)
{
	uint16_t	i, j;

	for (i = 0; i < r->nentries && r->entries; i++) {
		for (j = 0; j < r->entries[i].nattrs; j++)
			 free(r->entries[i].attrs[j].attr);
		free(r->entries[i].attrs);
		free(r->entries[i].aspath);
	}

	free(r->entries);
	free(r);
}

void
mrt_free_bgp_state(struct mrt_bgp_state *s)
{
	free(s);
}

void
mrt_free_bgp_msg(struct mrt_bgp_msg *m)
{
	free(m->msg);
	free(m);
}

u_char *
mrt_aspath_inflate(void *data, uint16_t len, uint16_t *newlen)
{
	uint8_t		*seg, *nseg, *ndata;
	uint16_t	 seg_size, olen, nlen;
	uint8_t		 seg_len;

	/* first calculate the length of the aspath */
	seg = data;
	nlen = 0;
	for (olen = len; olen > 0; olen -= seg_size, seg += seg_size) {
		seg_len = seg[1];
		seg_size = 2 + sizeof(uint16_t) * seg_len;
		nlen += 2 + sizeof(uint32_t) * seg_len;

		if (seg_size > olen)
			return NULL;
	}

	*newlen = nlen;
	if ((ndata = malloc(nlen)) == NULL)
		err(1, "malloc");

	/* then copy the aspath */
	seg = data;
	for (nseg = ndata; nseg < ndata + nlen; ) {
		*nseg++ = *seg++;
		*nseg++ = seg_len = *seg++;
		for (; seg_len > 0; seg_len--) {
			*nseg++ = 0;
			*nseg++ = 0;
			*nseg++ = *seg++;
			*nseg++ = *seg++;
		}
	}

	return (ndata);
}

int
mrt_extract_addr(void *msg, u_int len, struct bgpd_addr *addr, uint8_t aid)
{
	uint8_t	*b = msg;

	memset(addr, 0, sizeof(*addr));
	switch (aid) {
	case AID_INET:
		if (len < sizeof(struct in_addr))
			return (-1);
		addr->aid = aid;
		memcpy(&addr->v4, b, sizeof(struct in_addr));
		return sizeof(struct in_addr);
	case AID_INET6:
		if (len < sizeof(struct in6_addr))
			return (-1);
		addr->aid = aid;
		memcpy(&addr->v6, b, sizeof(struct in6_addr));
		return sizeof(struct in6_addr);
	case AID_VPN_IPv4:
		if (len < sizeof(uint64_t) + sizeof(struct in_addr))
			return (-1);
		addr->aid = aid;
		/* XXX labelstack and rd missing */
		memcpy(&addr->v4, b + sizeof(uint64_t),
		    sizeof(struct in_addr));
		return (sizeof(uint64_t) + sizeof(struct in_addr));
	case AID_VPN_IPv6:
		if (len < sizeof(uint64_t) + sizeof(struct in6_addr))
			return (-1);
		addr->aid = aid;
		/* XXX labelstack and rd missing */
		memcpy(&addr->v6, b + sizeof(uint64_t),
		    sizeof(struct in6_addr));
		return (sizeof(uint64_t) + sizeof(struct in6_addr));
	default:
		return (-1);
	}
}

int
mrt_extract_prefix(void *m, u_int len, uint8_t aid,
    struct bgpd_addr *prefix, uint8_t *prefixlen, int verbose)
{
	struct ibuf buf, *msg = &buf;
	int r;

	ibuf_from_buffer(msg, m, len); /* XXX */
	switch (aid) {
	case AID_INET:
		r = nlri_get_prefix(msg, prefix, prefixlen);
		break;
	case AID_INET6:
		r = nlri_get_prefix6(msg, prefix, prefixlen);
		break;
	case AID_VPN_IPv4:
		r = nlri_get_vpn4(msg, prefix, prefixlen, 0);
		break;
	case AID_VPN_IPv6:
		r = nlri_get_vpn6(msg, prefix, prefixlen, 0);
		break;
	default:
		if (verbose)
			printf("unknown prefix AID %d\n", aid);
		return -1;
	}
	if (r == -1 && verbose)
		printf("failed to parse prefix of AID %d\n", aid);
	if (r != -1)
		r = len - ibuf_size(msg); /* XXX */
	return r;
}

struct mrt_bgp_state *
mrt_parse_state(struct mrt_hdr *hdr, void *msg, int verbose)
{
	struct timespec		 t;
	struct mrt_bgp_state	*s;
	uint8_t			*b = msg;
	u_int			 len = ntohl(hdr->length);
	uint32_t		 sas, das, usec;
	uint16_t		 tmp16, afi;
	int			 r;
	uint8_t			 aid;

	t.tv_sec = ntohl(hdr->timestamp);
	t.tv_nsec = 0;

	/* handle the microsec field for _ET header */
	if (ntohs(hdr->type) == MSG_PROTOCOL_BGP4MP_ET) {
		memcpy(&usec, b, sizeof(usec));
		b += sizeof(usec);
		len -= sizeof(usec);
		t.tv_nsec = ntohl(usec) * 1000;
	}

	switch (ntohs(hdr->subtype)) {
	case BGP4MP_STATE_CHANGE:
		if (len < 8)
			return (0);
		/* source as */
		memcpy(&tmp16, b, sizeof(tmp16));
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		sas = ntohs(tmp16);
		/* dest as */
		memcpy(&tmp16, b, sizeof(tmp16));
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		das = ntohs(tmp16);
		/* if_index, ignored */
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		/* afi */
		memcpy(&tmp16, b, sizeof(tmp16));
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		afi = ntohs(tmp16);
		break;
	case BGP4MP_STATE_CHANGE_AS4:
		if (len < 12)
			return (0);
		/* source as */
		memcpy(&sas, b, sizeof(sas));
		b += sizeof(sas);
		len -= sizeof(sas);
		sas = ntohl(sas);
		/* dest as */
		memcpy(&das, b, sizeof(das));
		b += sizeof(das);
		len -= sizeof(das);
		das = ntohl(das);
		/* if_index, ignored */
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		/* afi */
		memcpy(&tmp16, b, sizeof(tmp16));
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		afi = ntohs(tmp16);
		break;
	default:
		errx(1, "mrt_parse_state: bad subtype");
	}

	/* src & dst addr */
	if ((aid = mrt_afi2aid(afi, -1, verbose)) == AID_UNSPEC)
		return (NULL);

	if ((s = calloc(1, sizeof(struct mrt_bgp_state))) == NULL)
		err(1, "calloc");
	s->time = t;
	s->src_as = sas;
	s->dst_as = das;

	if ((r = mrt_extract_addr(b, len, &s->src, aid)) == -1)
		goto fail;
	b += r;
	len -= r;
	if ((r = mrt_extract_addr(b, len, &s->dst, aid)) == -1)
		goto fail;
	b += r;
	len -= r;

	/* states */
	memcpy(&tmp16, b, sizeof(tmp16));
	b += sizeof(tmp16);
	len -= sizeof(tmp16);
	s->old_state = ntohs(tmp16);
	memcpy(&tmp16, b, sizeof(tmp16));
	b += sizeof(tmp16);
	len -= sizeof(tmp16);
	s->new_state = ntohs(tmp16);

	return (s);

fail:
	free(s);
	return (NULL);
}

struct mrt_bgp_msg *
mrt_parse_msg(struct mrt_hdr *hdr, void *msg, int verbose)
{
	struct timespec		 t;
	struct mrt_bgp_msg	*m;
	uint8_t			*b = msg;
	u_int			 len = ntohl(hdr->length);
	uint32_t		 sas, das, usec;
	uint16_t		 tmp16, afi;
	int			 r, addpath = 0;
	uint8_t			 aid;

	t.tv_sec = ntohl(hdr->timestamp);
	t.tv_nsec = 0;

	/* handle the microsec field for _ET header */
	if (ntohs(hdr->type) == MSG_PROTOCOL_BGP4MP_ET) {
		memcpy(&usec, b, sizeof(usec));
		b += sizeof(usec);
		len -= sizeof(usec);
		t.tv_nsec = ntohl(usec) * 1000;
	}

	switch (ntohs(hdr->subtype)) {
	case BGP4MP_MESSAGE_ADDPATH:
	case BGP4MP_MESSAGE_LOCAL_ADDPATH:
		addpath = 1;
		/* FALLTHROUGH */
	case BGP4MP_MESSAGE:
	case BGP4MP_MESSAGE_LOCAL:
		if (len < 8)
			return (0);
		/* source as */
		memcpy(&tmp16, b, sizeof(tmp16));
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		sas = ntohs(tmp16);
		/* dest as */
		memcpy(&tmp16, b, sizeof(tmp16));
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		das = ntohs(tmp16);
		/* if_index, ignored */
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		/* afi */
		memcpy(&tmp16, b, sizeof(tmp16));
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		afi = ntohs(tmp16);
		break;
	case BGP4MP_MESSAGE_AS4_ADDPATH:
	case BGP4MP_MESSAGE_AS4_LOCAL_ADDPATH:
		addpath = 1;
		/* FALLTHROUGH */
	case BGP4MP_MESSAGE_AS4:
	case BGP4MP_MESSAGE_AS4_LOCAL:
		if (len < 12)
			return (0);
		/* source as */
		memcpy(&sas, b, sizeof(sas));
		b += sizeof(sas);
		len -= sizeof(sas);
		sas = ntohl(sas);
		/* dest as */
		memcpy(&das, b, sizeof(das));
		b += sizeof(das);
		len -= sizeof(das);
		das = ntohl(das);
		/* if_index, ignored */
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		/* afi */
		memcpy(&tmp16, b, sizeof(tmp16));
		b += sizeof(tmp16);
		len -= sizeof(tmp16);
		afi = ntohs(tmp16);
		break;
	default:
		errx(1, "mrt_parse_msg: bad subtype");
	}

	/* src & dst addr */
	if ((aid = mrt_afi2aid(afi, -1, verbose)) == AID_UNSPEC)
		return (NULL);

	if ((m = calloc(1, sizeof(struct mrt_bgp_msg))) == NULL)
		err(1, "calloc");
	m->time = t;
	m->src_as = sas;
	m->dst_as = das;
	m->add_path = addpath;

	if ((r = mrt_extract_addr(b, len, &m->src, aid)) == -1)
		goto fail;
	b += r;
	len -= r;
	if ((r = mrt_extract_addr(b, len, &m->dst, aid)) == -1)
		goto fail;
	b += r;
	len -= r;

	/* msg */
	if (len > 0) {
		m->msg_len = len;
		if ((m->msg = malloc(len)) == NULL)
			err(1, "malloc");
		memcpy(m->msg, b, len);
	}

	return (m);

fail:
	free(m->msg);
	free(m);
	return (NULL);
}
