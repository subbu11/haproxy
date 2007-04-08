/*
 * FD polling functions for linux epoll()
 *
 * Copyright 2000-2007 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/time.h>

#include <types/fd.h>
#include <types/global.h>

#include <proto/fd.h>
#include <proto/polling.h>
#include <proto/task.h>

#if defined(USE_MY_EPOLL)
#include <errno.h>
#include <sys/syscall.h>
_syscall1 (int, epoll_create, int, size);
_syscall4 (int, epoll_ctl, int, epfd, int, op, int, fd, struct epoll_event *, event);
_syscall4 (int, epoll_wait, int, epfd, struct epoll_event *, events, int, maxevents, int, timeout);
#endif


static fd_set *StaticReadEvent, *StaticWriteEvent;
static fd_set *PrevReadEvent, *PrevWriteEvent;

/* private data */
static struct epoll_event *epoll_events;
static int epoll_fd;


/*
 * Benchmarks performed on a Pentium-M notebook show that using functions
 * instead of the usual macros improve the FD_* performance by about 80%,
 * and that marking them regparm(2) adds another 20%.
 */
REGPRM2 static int __fd_isset(const int fd, const int dir)
{
	fd_set *ev;
	if (dir == DIR_RD)
		ev = StaticReadEvent;
	else
		ev = StaticWriteEvent;

	return FD_ISSET(fd, ev);
}

REGPRM2 static void __fd_set(const int fd, const int dir)
{
	fd_set *ev;
	if (dir == DIR_RD)
		ev = StaticReadEvent;
	else
		ev = StaticWriteEvent;

	FD_SET(fd, ev);
}

REGPRM2 static void __fd_clr(const int fd, const int dir)
{
	fd_set *ev;
	if (dir == DIR_RD)
		ev = StaticReadEvent;
	else
		ev = StaticWriteEvent;

	FD_CLR(fd, ev);
}

REGPRM2 static int __fd_cond_s(const int fd, const int dir)
{
	int ret;
	fd_set *ev;
	if (dir == DIR_RD)
		ev = StaticReadEvent;
	else
		ev = StaticWriteEvent;

	ret = !FD_ISSET(fd, ev);
	if (ret)
		FD_SET(fd, ev);
	return ret;
}

REGPRM2 static int __fd_cond_c(const int fd, const int dir)
{
	int ret;
	fd_set *ev;
	if (dir == DIR_RD)
		ev = StaticReadEvent;
	else
		ev = StaticWriteEvent;

	ret = FD_ISSET(fd, ev);
	if (ret)
		FD_CLR(fd, ev);
	return ret;
}

REGPRM1 static void __fd_rem(const int fd)
{
	FD_CLR(fd, StaticReadEvent);
	FD_CLR(fd, StaticWriteEvent);
}

REGPRM1 static void __fd_clo(const int fd)
{
	FD_CLR(fd, StaticReadEvent);
	FD_CLR(fd, StaticWriteEvent);
	FD_CLR(fd, PrevReadEvent);
	FD_CLR(fd, PrevWriteEvent);
}



/*
 * Initialization of the epoll() poller.
 * Returns 0 in case of failure, non-zero in case of success. If it fails, it
 * disables the poller by setting its pref to 0.
 */
REGPRM1 static int epoll_init(struct poller *p)
{
	__label__ fail_pwevt, fail_prevt, fail_swevt, fail_srevt, fail_ee, fail_fd;
	int fd_set_bytes;

	p->private = NULL;
	fd_set_bytes = sizeof(fd_set) * (global.maxsock + FD_SETSIZE - 1) / FD_SETSIZE;

	epoll_fd = epoll_create(global.maxsock + 1);
	if (epoll_fd < 0)
		goto fail_fd;

	epoll_events = (struct epoll_event*)
		calloc(1, sizeof(struct epoll_event) * global.maxsock);

	if (epoll_events == NULL)
		goto fail_ee;

	if ((PrevReadEvent = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_prevt;

	if ((PrevWriteEvent = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_pwevt;
		
	if ((StaticReadEvent = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_srevt;

	if ((StaticWriteEvent = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_swevt;

	return 1;

 fail_swevt:
	free(StaticReadEvent);
 fail_srevt:
	free(PrevWriteEvent);
 fail_pwevt:
	free(PrevReadEvent);
 fail_prevt:
	free(epoll_events);
 fail_ee:
	close(epoll_fd);
	epoll_fd = 0;
 fail_fd:
	p->pref = 0;
	return 0;
}

/*
 * Termination of the epoll() poller.
 * Memory is released and the poller is marked as unselectable.
 */
REGPRM1 static void epoll_term(struct poller *p)
{
	if (StaticWriteEvent)
		free(StaticWriteEvent);

	if (StaticReadEvent)
		free(StaticReadEvent);

	if (PrevWriteEvent)
		free(PrevWriteEvent);

	if (PrevReadEvent)
		free(PrevReadEvent);

	if (epoll_events)
		free(epoll_events);

	close(epoll_fd);
	epoll_fd = 0;

	p->private = NULL;
	p->pref = 0;
}

/*
 * epoll() poller
 */
REGPRM2 static void epoll_poll(struct poller *p, int wait_time)
{
	int status;
	int fd;

	int fds, count;
	int pr, pw, sr, sw;
	unsigned rn, ro, wn, wo; /* read new, read old, write new, write old */
	struct epoll_event ev;

	for (fds = 0; (fds << INTBITS) < maxfd; fds++) {
	  
		rn = ((int*)StaticReadEvent)[fds];  ro = ((int*)PrevReadEvent)[fds];
		wn = ((int*)StaticWriteEvent)[fds]; wo = ((int*)PrevWriteEvent)[fds];
	  
		if ((ro^rn) | (wo^wn)) {
			for (count = 0, fd = fds << INTBITS; count < (1<<INTBITS) && fd < maxfd; count++, fd++) {
#define FDSETS_ARE_INT_ALIGNED
#ifdef FDSETS_ARE_INT_ALIGNED

#define WE_REALLY_NOW_THAT_FDSETS_ARE_INTS
#ifdef WE_REALLY_NOW_THAT_FDSETS_ARE_INTS
				pr = (ro >> count) & 1;
				pw = (wo >> count) & 1;
				sr = (rn >> count) & 1;
				sw = (wn >> count) & 1;
#else
				pr = FD_ISSET(fd&((1<<INTBITS)-1), (typeof(fd_set*))&ro);
				pw = FD_ISSET(fd&((1<<INTBITS)-1), (typeof(fd_set*))&wo);
				sr = FD_ISSET(fd&((1<<INTBITS)-1), (typeof(fd_set*))&rn);
				sw = FD_ISSET(fd&((1<<INTBITS)-1), (typeof(fd_set*))&wn);
#endif
#else
				pr = FD_ISSET(fd, PrevReadEvent);
				pw = FD_ISSET(fd, PrevWriteEvent);
				sr = FD_ISSET(fd, StaticReadEvent);
				sw = FD_ISSET(fd, StaticWriteEvent);
#endif
				if (!((sr^pr) | (sw^pw)))
					continue;

				ev.events = (sr ? EPOLLIN : 0) | (sw ? EPOLLOUT : 0);
				ev.data.fd = fd;

#ifdef EPOLL_CTL_MOD_WORKAROUND
				/* I encountered a rarely reproducible problem with
				 * EPOLL_CTL_MOD where a modified FD (systematically
				 * the one in epoll_events[0], fd#7) would sometimes
				 * be set EPOLL_OUT while asked for a read ! This is
				 * with the 2.4 epoll patch. The workaround is to
				 * delete then recreate in case of modification.
				 * This is in 2.4 up to epoll-lt-0.21 but not in 2.6
				 * nor RHEL kernels.
				 */

				if ((pr | pw) && fdtab[fd].state != FD_STCLOSE)
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev);

				if ((sr | sw))
					epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
#else
				if ((pr | pw)) {
					/* the file-descriptor already exists... */
					if ((sr | sw)) {
						/* ...and it will still exist */
						if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
							// perror("epoll_ctl(MOD)");
							// exit(1);
						}
					} else {
						/* ...and it will be removed */
						if (fdtab[fd].state != FD_STCLOSE &&
						    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev) < 0) {
							// perror("epoll_ctl(DEL)");
							// exit(1);
						}
					}
				} else {
					/* the file-descriptor did not exist, let's add it */
					if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
						// perror("epoll_ctl(ADD)");
						//  exit(1);
					}
				}
#endif // EPOLL_CTL_MOD_WORKAROUND
			}
			((int*)PrevReadEvent)[fds] = rn;
			((int*)PrevWriteEvent)[fds] = wn;
		}		  
	}
      
	/* now let's wait for events */
	status = epoll_wait(epoll_fd, epoll_events, maxfd, wait_time);
	tv_now(&now);

	for (count = 0; count < status; count++) {
		fd = epoll_events[count].data.fd;

		if (FD_ISSET(fd, StaticReadEvent)) {
			if (fdtab[fd].state == FD_STCLOSE)
				continue;
			if (epoll_events[count].events & ( EPOLLIN | EPOLLERR | EPOLLHUP ))
				fdtab[fd].cb[DIR_RD].f(fd);
		}

		if (FD_ISSET(fd, StaticWriteEvent)) {
			if (fdtab[fd].state == FD_STCLOSE)
				continue;
			if (epoll_events[count].events & ( EPOLLOUT | EPOLLERR | EPOLLHUP ))
				fdtab[fd].cb[DIR_WR].f(fd);
		}
	}
}

/*
 * The only exported function. Returns 1.
 */
int epoll_register(struct poller *p)
{
	p->name = "epoll";
	p->pref = 300;
	p->private = NULL;

	p->init = epoll_init;
	p->term = epoll_term;
	p->poll = epoll_poll;
	p->isset = __fd_isset;
	p->set = __fd_set;
	p->clr = __fd_clr;
	p->rem = __fd_rem;
	p->clo = __fd_clo;
	p->cond_s = __fd_cond_s;
	p->cond_c = __fd_cond_c;
	return 1;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
