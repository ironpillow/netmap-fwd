/*-
 * Copyright (c) 2015, Luiz Otavio O Souza <loos@FreeBSD.org>
 * Copyright (c) 2015, Rubicon Communications, LLC (Netgate)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/types.h>

#include <net/ethernet.h>
#include <netinet/in.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "counters.h"
#include "ether.h"
#include "event.h"
#include "if.h"
#include "netmap.h"
#include "util.h"

extern int burst;
extern int nohostring;

static void
netmap_read(evutil_socket_t fd, short event, void *data)
{
	char *buf;
	int err, i, pkts, rx_rings;
	struct netmap_if *ifp;
	struct netmap_ring *nring;
	struct nm_if *nmif;

	nmif = (struct nm_if *)data;
	ifp = nmif->nm_if_ifp;
	rx_rings = ifp->ni_rx_rings;
	if (!nohostring && !nmif->nm_if_vale)
		rx_rings++;
	pkts = 0;
	for (i = 0; i < rx_rings; i++) {
		nring = NETMAP_RXRING(ifp, i);
		while (!nm_ring_empty(nring)) {
			buf = NETMAP_GET_BUF(nring);
			err = ether_input(nmif, i, buf, NETMAP_SLOT_LEN(nring));
			/* Send the packet to hw <-> host bridge. */
			if (!nohostring && err == 1)
				err = ether_bridge(nmif, i, buf,
				    NETMAP_SLOT_LEN(nring));
			NETMAP_RING_NEXT(nring);
			if (err < 0 || ++pkts == burst)
				goto done;
		}
	}
done:
	if_netmap_txsync();
}

static int
netmap_vale_cmd(const char *ifname, uint16_t cmd)
{
	int err, fd;
	struct nmreq nmreq;

	fd = open("/dev/netmap", O_RDWR);
	if (fd == -1) {
		perror("open");
		return (-1);
	}
	memset(&nmreq, 0, sizeof(nmreq));
	strlcpy(nmreq.nr_name, ifname, sizeof(nmreq.nr_name));
	nmreq.nr_version = NETMAP_API;
	nmreq.nr_cmd = cmd;
	nmreq.nr_flags = NR_REG_ALL_NIC;
	err = ioctl(fd, NIOCREGIF, &nmreq);
	if (err == -1)
		perror("ioctl");
	close(fd);

	return (err);
}

static int
netmap_vale_attach(struct nm_if *nmif)
{

	return (netmap_vale_cmd(nmif->nm_if_name, NETMAP_BDG_ATTACH));
}

static int
netmap_vale_detach(struct nm_if *nmif)
{

	return (netmap_vale_cmd(nmif->nm_if_name, NETMAP_BDG_DETACH));
}

int
netmap_open(struct nm_if *nmif)
{
	char ifbuf[IF_NAMESIZE], *p;
	const char *ifname;
	int len;
	struct nmreq nmreq;

	if (nmif->nm_if_vale) {
		/* Attach hw interface to VALE switch. */
		if (netmap_vale_attach(nmif) != 0) {
			netmap_close(nmif);
			return (-1);
		}
		/* Attach netmap-fwd to VALE switch. */
		p = strchr(nmif->nm_if_name, ':');
		len = 0;
		if (p)
			len = p - nmif->nm_if_name;
		memset(ifbuf, 0, sizeof(ifbuf));
		snprintf(ifbuf, sizeof(ifbuf) - 1, "%.*s:nmfwd0", len, nmif->nm_if_name);
		ifname = ifbuf;
	} else
		ifname = nmif->nm_if_name;

	nmif->nm_if_fd = open("/dev/netmap", O_RDWR);
	if (nmif->nm_if_fd == -1) {
		perror("open");
		return (-1);
	}

	memset(&nmreq, 0, sizeof(nmreq));
	strlcpy(nmreq.nr_name, ifname, sizeof(nmreq.nr_name));
	nmreq.nr_version = NETMAP_API;
	if (nohostring || nmif->nm_if_vale)
		nmreq.nr_flags = NR_REG_ALL_NIC;
	else
		nmreq.nr_flags = NR_REG_NIC_SW;
	if (nmif->nm_if_vale)
		nmreq.nr_tx_rings = nmreq.nr_rx_rings = 4;
	if (ioctl(nmif->nm_if_fd, NIOCREGIF, &nmreq) == -1) {
		perror("ioctl");
		netmap_close(nmif);
		return (-1);
	}
DPRINTF("fd: %d\n", nmif->nm_if_fd);
DPRINTF("name: %s\n", nmreq.nr_name);
DPRINTF("version: %d\n", nmreq.nr_version);
DPRINTF("offset: %d\n", nmreq.nr_offset);
DPRINTF("memsize: %d\n", nmreq.nr_memsize);
DPRINTF("tx_slots: %d\n", nmreq.nr_tx_slots);
DPRINTF("rx_slots: %d\n", nmreq.nr_rx_slots);
DPRINTF("tx_rings: %d\n", nmreq.nr_tx_rings);
DPRINTF("rx_rings: %d\n", nmreq.nr_rx_rings);
DPRINTF("ringid: %#x\n", nmreq.nr_ringid);
DPRINTF("flags: %#x\n", nmreq.nr_flags);
	nmif->nm_if_memsize = nmreq.nr_memsize;
	nmif->nm_if_mem = mmap(NULL, nmif->nm_if_memsize,
	    PROT_READ | PROT_WRITE, MAP_SHARED, nmif->nm_if_fd, 0);
	if (nmif->nm_if_mem == MAP_FAILED) {
		perror("mmap");
		netmap_close(nmif);
		return (-1);
	}
	nmif->nm_if_ifp = NETMAP_IF(nmif->nm_if_mem, nmreq.nr_offset);
	nmif->nm_if_ev_read = event_new(ev_get_base(), nmif->nm_if_fd,
	    EV_READ | EV_PERSIST, netmap_read, nmif);
	event_add(nmif->nm_if_ev_read, NULL);

	return (0);
}

int
netmap_close(struct nm_if *nmif)
{

	if (nmif->nm_if_ev_read != NULL) {
		event_del(nmif->nm_if_ev_read);
		event_free(nmif->nm_if_ev_read);
		nmif->nm_if_ev_read = NULL;
	}
	if (nmif->nm_if_mem != NULL && nmif->nm_if_memsize > 0) {
		munmap(nmif->nm_if_mem, nmif->nm_if_memsize);
		nmif->nm_if_mem = NULL;
		nmif->nm_if_memsize = 0;
	}
	if (nmif->nm_if_fd == -1)
		return (0);
	if (close(nmif->nm_if_fd) == -1) {
		perror("close");
		return (-1);
	}
	nmif->nm_if_fd = -1;

	/* Detach hw interface from VALE switch. */
	if (nmif->nm_if_vale)
		netmap_vale_detach(nmif);

	return (0);
}

int
netmap_tx_sync(struct nm_if *nmif)
{

	if (ioctl(nmif->nm_if_fd, NIOCTXSYNC, NULL) == -1) {
		perror("ioctl");
		return (-1);
	}
	nmif->nm_if_txsync = 0;

	return (0);
}
