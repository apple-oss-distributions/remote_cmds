/*	$NetBSD: tftp.c,v 1.18 2003/08/07 11:16:14 agc Exp $	*/

/*
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if 0
#ifndef lint
static char sccsid[] = "@(#)tftp.c	8.1 (Berkeley) 6/6/93";
#endif /* not lint */
#endif

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/usr.bin/tftp/tftp.c,v 1.13 2006/09/28 21:22:21 matteo Exp $");

/* Many bug fixes are from Jim Guyton <guyton@rand-unix> */

/*
 * TFTP User Program -- Protocol Machines
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <netinet/in.h>

#include <arpa/inet.h>
#include <arpa/tftp.h>

#include <err.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#ifdef __APPLE__
#include <stdlib.h>
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>

#include "extern.h"
#include "tftpsubs.h"

extern  struct sockaddr_storage peeraddr; /* filled in by main */
extern  int     f;			/* the opened socket */
extern  int     trace;
extern  int     verbose;
extern  int     def_rexmtval;
extern  int     rexmtval;
extern  int     maxtimeout;
extern  volatile int txrx_error;
#ifdef __APPLE__
extern	int	tsize;
extern	int	tout;
extern	int	def_blksize;
extern	int	blksize;
#endif

char    ackbuf[PKTSIZE];
int	timeout;
extern jmp_buf	toplevel;
jmp_buf	timeoutbuf;

static void nak(int, struct sockaddr *);
#ifdef __APPLE__
static int makerequest(int, const char *, struct tftphdr *, const char *, off_t);
#else
static int makerequest(int, const char *, struct tftphdr *, const char *);
#endif
static void printstats(const char *, unsigned long);
static void startclock(void);
static void stopclock(void);
static void timer(int);
static void tpacket(const char *, struct tftphdr *, int);
static int sockaddrcmp(struct sockaddr *, struct sockaddr *);

static void get_options(struct tftphdr *, int);

static void
get_options(struct tftphdr *ap, int size)
{
	unsigned long val;
	char *opt, *endp, *nextopt, *valp;
	int l;

	size -= 2;	/* skip over opcode */
	opt = ap->th_stuff;
	endp = opt + size - 1;
	*endp = '\0';
	
	while (opt < endp) {
		l = strlen(opt) + 1;
		valp = opt + l;
		if (valp < endp) {
			val = strtoul(valp, NULL, 10);
			l = strlen(valp) + 1;
			nextopt = valp + l;
			if (val == ULONG_MAX && errno == ERANGE) {
				/* Report illegal value */
				opt = nextopt;
				continue;
			}
		} else {
			/* Badly formed OACK */
			break;
		}
		if (strcmp(opt, "tsize") == 0) {
			/* cool, but we'll ignore it */
		} else if (strcmp(opt, "timeout") == 0) {
			if (val >= 1 && val <= 255) {
				rexmtval = val;
			} else {
				/* Report error? */
			}
		} else if (strcmp(opt, "blksize") == 0) {
			if (val >= 8 && val <= MAXSEGSIZE) {
				blksize = val;
			} else {
				/* Report error? */
			}
		} else {
			/* unknown option */
		}
		opt = nextopt;
	}
}

/*
 * Send the requested file.
 */
void
xmitfile(fd, name, mode)
	int fd;
	char *name;
	char *mode;
{
	struct tftphdr *ap;	   /* data and ack packets */
	struct tftphdr *dp;
	int n;
	volatile unsigned int block;
	volatile int size, convert;
	volatile unsigned long amount;
	struct sockaddr_storage from;
	struct stat sbuf;
	off_t filesize=0;
	socklen_t fromlen;
	FILE *file;
	struct sockaddr_storage peer;
	struct sockaddr_storage serv;	/* valid server port number */

	startclock();		/* start stat's clock */
	dp = r_init();		/* reset fillbuf/read-ahead code */
	ap = (struct tftphdr *)ackbuf;
	if (tsize) {
		if (fstat(fd, &sbuf) == 0) {
			filesize = sbuf.st_size;
		} else {
			filesize = -1ULL;
		}
	}
	file = fdopen(fd, "r");
	convert = !strcmp(mode, "netascii");
	block = 0;
	amount = 0;
	memcpy(&peer, &peeraddr, peeraddr.ss_len);
	memset(&serv, 0, sizeof(serv));

	signal(SIGALRM, timer);
	do {
		if (block == 0)
			size = makerequest(WRQ, name, dp, mode, filesize) - 4;
		else {
		/*	size = read(fd, dp->th_data, SEGSIZE);	 */
			size = readit(file, &dp, blksize, convert);
			if (size < 0) {
				nak(errno + 100, (struct sockaddr *)&peer);
				break;
			}
			dp->th_opcode = htons((u_short)DATA);
			dp->th_block = htons((u_short)block);
		}
		timeout = 0;
		(void) setjmp(timeoutbuf);
send_data:
		if (trace)
			tpacket("sent", dp, size + 4);
		n = sendto(f, dp, size + 4, 0,
		    (struct sockaddr *)&peer, peer.ss_len);
		if (n != size + 4) {
			warn("sendto");
			txrx_error = 1;
			goto abort;
		}
		if (block)
			read_ahead(file, blksize, convert);
		for ( ; ; ) {
			alarm(rexmtval);
			do {
				fromlen = sizeof(from);
				n = recvfrom(f, ackbuf, sizeof(ackbuf), 0,
				    (struct sockaddr *)&from, &fromlen);
			} while (n <= 0);
			alarm(0);
			if (n < 0) {
				warn("recvfrom");
				txrx_error = 1;
				goto abort;
			}
			if (!serv.ss_family)
				serv = from;
			else if (!sockaddrcmp((struct sockaddr *)&serv,
					      (struct sockaddr *)&from)) {
				warn("server address/port mismatch");
				txrx_error = 1;
				goto abort;
			}
			peer = from;
			if (trace)
				tpacket("received", ap, n);
			/* should verify packet came from server */
			ap->th_opcode = ntohs(ap->th_opcode);
			if (ap->th_opcode == ERROR) {
				printf("Error code %d: %s\n", ap->th_code,
					ap->th_msg);
				txrx_error = 1;
				goto abort;
			}
			if (ap->th_opcode == ACK) {
				int j;

				ap->th_block = ntohs(ap->th_block);
				if (ap->th_block == 0 && block == 0) {
					/*
					 * If the extended options are enabled,
					 * the server just refused 'em all.
					 * The only one that _really_
					 * matters is blksize, but we'll
					 * clear timeout, too.
					 */
					blksize = def_blksize;
					rexmtval = def_rexmtval;
				}
				if (ap->th_block == (u_short)block) {
					break;
				}
				/* On an error, try to synchronize
				 * both sides.
				 */
				j = synchnet(f);
				if (j && trace) {
					printf("discarded %d packets\n",
							j);
				}
				if (ap->th_block == (u_short)(block-1)) {
					goto send_data;
				}
			}
			if (ap->th_opcode == OACK) {
				if (block == 0) {
					blksize = def_blksize;
					rexmtval = def_rexmtval;
					get_options(ap, n);
					break;
				}
			}
		}
		if (block > 0)
			amount += size;
		block++;
	} while (size == blksize || block == 1);
abort:
	fclose(file);
	stopclock();
	if (amount > 0)
		printstats("Sent", amount);
}

/*
 * Receive a file.
 */
void
recvfile(fd, name, mode)
	int fd;
	char *name;
	char *mode;
{
	struct tftphdr *ap;
	struct tftphdr *dp;
	int n, oack=0;
	volatile unsigned int block;
	volatile int size, firsttrip;
	volatile unsigned long amount;
	struct sockaddr_storage from;
	socklen_t fromlen;
	int readlen;
	FILE *file;
	volatile int convert;		/* true if converting crlf -> lf */
	struct sockaddr_storage peer;
	struct sockaddr_storage serv;	/* valid server port number */

	startclock();
	dp = w_init();
	ap = (struct tftphdr *)ackbuf;
	file = fdopen(fd, "w");
	convert = !strcmp(mode, "netascii");
	block = 1;
	firsttrip = 1;
	amount = 0;
	memcpy(&peer, &peeraddr, peeraddr.ss_len);
	memset(&serv, 0, sizeof(serv));

	signal(SIGALRM, timer);
	do {
		short rx_opcode;

		if (firsttrip) {
			size = makerequest(RRQ, name, ap, mode, 0);
			readlen = PKTSIZE;
			firsttrip = 0;
		} else {
			ap->th_opcode = htons((u_short)ACK);
			ap->th_block = htons((u_short)(block));
			readlen = blksize+4;
			size = 4;
			block++;
		}
		timeout = 0;
		(void) setjmp(timeoutbuf);
send_ack:
		if (trace)
			tpacket("sent", ap, size);
		if (sendto(f, ackbuf, size, 0, (struct sockaddr *)&peer,
		    peer.ss_len) != size) {
			alarm(0);
			warn("sendto");
			txrx_error = 1;
			goto abort;
		}
		write_behind(file, convert);
		for ( ; ; ) {
			int j;
			unsigned short rx_block;

			alarm(rexmtval);
			do  {
				fromlen = sizeof(from);
				n = recvfrom(f, dp, readlen, 0,
				    (struct sockaddr *)&from, &fromlen);
			} while (n <= 0);
			alarm(0);
			if (n < 0) {
				warn("recvfrom");
				txrx_error = 1;
				goto abort;
			}
			if (serv.ss_family != AF_UNSPEC
			    && !sockaddrcmp((struct sockaddr *)&serv,
					    (struct sockaddr *)&from)) {
				/* ignore this packet */
				if (trace && verbose)
					tpacket("ignored", dp, n);
				continue;
			}
			if (trace)
				tpacket("received", dp, n);
			/* should verify client address */
			rx_opcode = ntohs(dp->th_opcode);
			switch (rx_opcode) {
			case ERROR:
				printf("Error code %d: %s\n", dp->th_code,
					dp->th_msg);
				txrx_error = 1;
				if (serv.ss_family == AF_UNSPEC) {
					peer = from;
				}
				txrx_error = 1;
				goto abort;
			case DATA:
				rx_block = ntohs(dp->th_block);
				if (rx_block == (u_short)block) {
					if (block == 1 && !oack) {
						/* no OACK, revert to defaults */
						blksize = def_blksize;
						rexmtval = def_rexmtval;
					}
					if (serv.ss_family == AF_UNSPEC) {
						peer = serv = from;
					}
					goto next_packet;
				}
				/* On an error, try to synchronize
				 * both sides.
				 */
				j = synchnet(f);
				if (j && trace) {
					printf("discarded %d packets\n", j);
				}
				if (rx_block == (u_short)(block - 1)
				    && serv.ss_family != AF_UNSPEC) {
					goto send_ack;	/* resend ack */
				}
				break;
			case OACK:
				if (block == 1) {
					if (serv.ss_family == AF_UNSPEC) {
						serv = peer = from;
					}
					oack = 1;
					blksize = def_blksize;
					rexmtval = def_rexmtval;
					get_options(dp, n);
					ap->th_opcode = htons(ACK);
					ap->th_block = 0;
					readlen = blksize+4;
					size = 4;
					goto send_ack;
				}
				break;
			default:
				break;
			}
		}
next_packet:
	/*	size = write(fd, dp->th_data, n - 4); */
		size = writeit(file, &dp, n - 4, convert);
		if (size < 0) {
			nak(errno + 100, (struct sockaddr *)&peer);
			break;
		}
		amount += size;
	} while (size == blksize);
	goto done;
abort:						/* ok to ack, since user */
	ap->th_opcode = htons((u_short)ACK);	/* has seen err msg */
	ap->th_block = htons((u_short)block);
	(void) sendto(f, ackbuf, 4, 0, (struct sockaddr *)&peer,
	    peer.ss_len);
done:
	write_behind(file, convert);		/* flush last buffer */
	fclose(file);
	stopclock();
	if (amount > 0)
		printstats("Received", amount);
}

static int
#ifdef __APPLE__
makerequest(request, name, tp, mode, filesize)
#else
makerequest(request, name, tp, mode)
#endif
	int request;
	const char *name;
	struct tftphdr *tp;
	const char *mode;
#ifdef __APPLE__
	off_t filesize;
#endif
{
	char *cp;

	tp->th_opcode = htons((u_short)request);
	cp = tp->th_stuff;
	strcpy(cp, name);
	cp += strlen(name);
	*cp++ = '\0';
	strcpy(cp, mode);
	cp += strlen(mode);
	*cp++ = '\0';
#ifdef __APPLE__
	if (tsize) {
		strcpy(cp, "tsize");
		cp += strlen(cp);
		*cp++ = '\0';
		sprintf(cp, "%lu", (unsigned long) filesize);
		cp += strlen(cp);
		*cp++ = '\0';
	}
	if (tout) {
		strcpy(cp, "timeout");
		cp += strlen(cp);
		*cp++ = '\0';
		sprintf(cp, "%d", rexmtval);
		cp += strlen(cp);
		*cp++ = '\0';
	}
	if (blksize != SEGSIZE) {
		strcpy(cp, "blksize");
		cp += strlen(cp);
		*cp++ = '\0';
		sprintf(cp, "%d", blksize);
		cp += strlen(cp);
		*cp++ = '\0';
	}
#endif
	return (cp - (char *)tp);
}

struct errmsg {
	int	e_code;
	const char	*e_msg;
} errmsgs[] = {
	{ EUNDEF,	"Undefined error code" },
	{ ENOTFOUND,	"File not found" },
	{ EACCESS,	"Access violation" },
	{ ENOSPACE,	"Disk full or allocation exceeded" },
	{ EBADOP,	"Illegal TFTP operation" },
	{ EBADID,	"Unknown transfer ID" },
	{ EEXISTS,	"File already exists" },
	{ ENOUSER,	"No such user" },
	{ EOPTNEG,	"Option negotiation failed" },
	{ -1,		0 }
};

/*
 * Send a nak packet (error message).
 * Error code passed in is one of the
 * standard TFTP codes, or a UNIX errno
 * offset by 100.
 */
static void
nak(error, peer)
	int error;
	struct sockaddr *peer;
{
	struct errmsg *pe;
	struct tftphdr *tp;
	int length;
	size_t msglen;

	tp = (struct tftphdr *)ackbuf;
	tp->th_opcode = htons((u_short)ERROR);
	msglen = sizeof(ackbuf) - (&tp->th_msg[0] - ackbuf);
	for (pe = errmsgs; pe->e_code >= 0; pe++)
		if (pe->e_code == error)
			break;
	if (pe->e_code < 0) {
		tp->th_code = EUNDEF;
		strlcpy(tp->th_msg, strerror(error - 100), msglen);
	} else {
		tp->th_code = htons((u_short)error);
		strlcpy(tp->th_msg, pe->e_msg, msglen);
	}
	length = strlen(tp->th_msg);
	msglen = &tp->th_msg[length + 1] - ackbuf;
	if (trace)
		tpacket("sent", tp, (int)msglen);
	if (sendto(f, ackbuf, msglen, 0, peer, peer->sa_len) != msglen)
		warn("nak");
}

static void
tpacket(s, tp, n)
	const char *s;
	struct tftphdr *tp;
	int n;
{
	static const char *opcodes[] =
	   { "#0", "RRQ", "WRQ", "DATA", "ACK", "ERROR", "OACK" };
	char *cp, *file, *endp, *opt = NULL, *spc;
	u_short op = ntohs(tp->th_opcode);
	int i, o;

	if (op < RRQ || op > OACK)
		printf("%s opcode=%x ", s, op);
	else
		printf("%s %s ", s, opcodes[op]);
	switch (op) {

	case RRQ:
	case WRQ:
		n -= 2;
		cp = tp->th_stuff;
		endp = cp + n - 1;
		if (*endp != '\0') {	/* Shouldn't happen, but... */
			*endp = '\0';
		}
		file = cp;
		cp = strchr(cp, '\0') + 1;
		printf("<file=%s, mode=%s", file, cp);
		cp = strchr(cp, '\0') + 1;
		o = 0;
		while (cp < endp) {
			i = strlen(cp) + 1;
			if (o) {
				printf(", %s=%s", opt, cp);
			} else {
				opt = cp;
			}
			o = (o+1) % 2;
			cp += i;
		}
		printf(">\n");
		break;

	case DATA:
		printf("<block=%d, %d bytes>\n", ntohs(tp->th_block), n - 4);
		break;

	case ACK:
		printf("<block=%d>\n", ntohs(tp->th_block));
		break;

	case ERROR:
		printf("<code=%d, msg=%s>\n", ntohs(tp->th_code), tp->th_msg);
		break;

	case OACK:
		o = 0;
		n -= 2;
		cp = tp->th_stuff;
		endp = cp + n - 1;
		if (*endp != '\0') {	/* Shouldn't happen, but... */
			*endp = '\0';
		}
		printf("<");
		spc = "";
		while (cp < endp) {
			i = strlen(cp) + 1;
			if (o) {
				printf("%s%s=%s", spc, opt, cp);
				spc = ", ";
			} else {
				opt = cp;
			}
			o = (o+1) % 2;
			cp += i;
		}
		printf(">\n");
		break;
	}
}

struct timeval tstart;
struct timeval tstop;

static void
startclock()
{

	(void)gettimeofday(&tstart, NULL);
}

static void
stopclock()
{

	(void)gettimeofday(&tstop, NULL);
}

static void
printstats(direction, amount)
	const char *direction;
	unsigned long amount;
{
	double delta;
			/* compute delta in 1/10's second units */
	delta = ((tstop.tv_sec*10.)+(tstop.tv_usec/100000)) -
		((tstart.tv_sec*10.)+(tstart.tv_usec/100000));
	delta = delta/10.;      /* back to seconds */
	printf("%s %ld bytes in %.1f seconds", direction, amount, delta);
	if (verbose)
		printf(" [%.0f bits/sec]", (amount*8.)/delta);
	putchar('\n');
}

static void
timer(sig)
	int sig __unused;
{

	timeout += rexmtval;
	if (timeout >= maxtimeout) {
		printf("Transfer timed out.\n");
		longjmp(toplevel, -1);
	}
        txrx_error = 1;
	longjmp(timeoutbuf, 1);
}

typedef union {
	struct sockaddr *	addr;
	struct sockaddr_in *	in;
	struct sockaddr_in6 *	in6;
} sockaddr_union;

static int
sockaddrcmp(struct sockaddr * sa, struct sockaddr * sb)
{
	sockaddr_union		a;
	sockaddr_union		b;

	if (sa->sa_len != sb->sa_len
	    || sa->sa_family != sb->sa_family) {
		return 0;
	}
	a.addr = sa;
	b.addr = sb;
	switch (sa->sa_family) {
	case AF_INET:
		if (a.in->sin_port != b.in->sin_port
		    || a.in->sin_addr.s_addr != b.in->sin_addr.s_addr) {
			return 0;
		}
		break;
	case AF_INET6:
		if (a.in6->sin6_port != b.in6->sin6_port
		    || !IN6_ARE_ADDR_EQUAL(&a.in6->sin6_addr,
					   &b.in6->sin6_addr)) {
			return 0;
		}
		break;
	default:
		if (bcmp(sa, sb, sa->sa_len) != 0) {
			return 0;
		}
		break;
	}
	return 1;
}
