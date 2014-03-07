/*
 * Copyright (c) 2013 Mellanox Technologies®. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies® BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies® nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "xio_os.h"

#include "xio_workqueue.h"
#include "xio_log.h"
#include "xio_observer.h"
#include "xio_context.h"
#include "xio_timers_list.h"

enum xio_workqueue_flags {
	XIO_WORKQUEUE_IN_POLL		= 1 << 0,
	XIO_WORKQUEUE_TIMER_ARMED	= 1 << 1
};

struct xio_workqueue {
	struct xio_context		*ctx;
	struct xio_timers_list		timers_list;
	int				timer_fd;
	int				pipe_fd[2];
	volatile uint32_t		flags;
};

#define NSEC_PER_SEC    1000000000L

/**
 * set_normalized_timespec - set timespec sec and nsec parts and
 * normalize
 *
 * @ts:         pointer to timespec variable to be set
 * @sec:        seconds to set
 * @nsec:       nanoseconds to set
 *
 * Set seconds and nanoseconds field of a timespec variable and
 * normalize to the timespec storage format
 *
 * Note: The tv_nsec part is always in the range of
 *      0 <= tv_nsec < NSEC_PER_SEC
 * For negative values only the tv_sec field is negative !
 */
static void set_normalized_timespec(struct timespec *ts,
				    time_t sec, int64_t nsec)
{
	while (nsec >= XIO_NS_IN_SEC) {
		nsec -= XIO_NS_IN_SEC;
		++sec;
	}
	while (nsec < 0) {
		nsec += XIO_NS_IN_SEC;
		--sec;
	}
	ts->tv_sec = sec;
	ts->tv_nsec = nsec;
}

/*---------------------------------------------------------------------------*/
/* xio_workqueue_rearm							     */
/*---------------------------------------------------------------------------*/
static int xio_workqueue_rearm(struct xio_workqueue *work_queue)
{
	struct itimerspec new_t = { {0, 0}, {0, 0} };
	int		  err;
	int64_t		  ns_to_expire;


	if (work_queue->flags & XIO_WORKQUEUE_IN_POLL)
		return 0;
	if (xio_timers_list_is_empty(&work_queue->timers_list))
		return 0;

	ns_to_expire =
		xio_timerlist_ns_duration_to_expire(
			&work_queue->timers_list);

	if (ns_to_expire == -1)
		return 0;

	if (ns_to_expire < 1) {
		new_t.it_value.tv_nsec = 1;
	} else {
		set_normalized_timespec(&new_t.it_value,
					0, ns_to_expire);
	}

	/* rearm the timer */
	err = timerfd_settime(work_queue->timer_fd, 0, &new_t, NULL);
	if (err < 0) {
		ERROR_LOG("timerfd_settime failed. %m\n");
		return -1;
	}

	work_queue->flags |= XIO_WORKQUEUE_TIMER_ARMED;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_workqueue_disarm							     */
/*---------------------------------------------------------------------------*/
static void xio_workqueue_disarm(struct xio_workqueue *work_queue)
{
	struct itimerspec new_t = { {0, 0}, {0, 0} };
	int		  err;

	if (!(work_queue->flags & XIO_WORKQUEUE_TIMER_ARMED))
		return;

	err = timerfd_settime(work_queue->timer_fd, 0, &new_t, NULL);
	if (err < 0)
		ERROR_LOG("timerfd_settime failed. %m\n");

	work_queue->flags &= ~XIO_WORKQUEUE_TIMER_ARMED;
}

/*---------------------------------------------------------------------------*/
/* xio_delayed_action_handler						     */
/*---------------------------------------------------------------------------*/
static void xio_delayed_action_handler(int fd, int events, void *user_context)
{
	struct xio_workqueue	*work_queue = user_context;
	int64_t			exp;
	ssize_t			s;

	/* consume the timer data in fd */
	s = read(work_queue->timer_fd, &exp, sizeof(exp));
	if (s < 0) {
		if (errno != EAGAIN)
			ERROR_LOG("failed to read from timerfd, %m\n");
		return;
	}
	if (s != sizeof(uint64_t)) {
		ERROR_LOG("failed to read from timerfd, %m\n");
		return;
	}


	work_queue->flags |= XIO_WORKQUEUE_IN_POLL;
	xio_timers_list_expire(&work_queue->timers_list);
	xio_timers_list_lock(&work_queue->timers_list);
	work_queue->flags &= ~XIO_WORKQUEUE_IN_POLL;
	xio_workqueue_rearm(work_queue);
	xio_timers_list_unlock(&work_queue->timers_list);
}

/*---------------------------------------------------------------------------*/
/* xio_work_action_handler						     */
/*---------------------------------------------------------------------------*/
static void xio_work_action_handler(int fd, int events, void *user_context)
{
	struct xio_workqueue	*work_queue = user_context;
	int64_t			exp;
	ssize_t			s;
	xio_work_handle_t	*work;

	/* drain the pipe data */
	while (1) {
		s = read(work_queue->pipe_fd[0], &exp, sizeof(exp));
		if (s < 0) {
			if (errno != EAGAIN)
				ERROR_LOG("failed to read from pipe, %m\n");
			return;
		}
		if (s != sizeof(uint64_t)) {
			ERROR_LOG("failed to read from pipe, %m\n");
			return;
		}
		work = ptr_from_int64(exp);

		if (work->flags & XIO_WORK_PENDING) {
			work->flags	&= ~XIO_WORK_PENDING;

			work->function(work->data);
		}
	}
}

/*---------------------------------------------------------------------------*/
/* xio_workqueue_create							     */
/*---------------------------------------------------------------------------*/
struct xio_workqueue *xio_workqueue_create(struct xio_context *ctx)
{
	struct xio_workqueue	*work_queue;
	int			retval;

	work_queue = ucalloc(1, sizeof(*work_queue));
	if (work_queue == NULL) {
		ERROR_LOG("ucalloc failed. %m\n");
		return NULL;
	}

	xio_timers_list_init(&work_queue->timers_list);
	work_queue->ctx = ctx;

	work_queue->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (work_queue->timer_fd < 0) {
		ERROR_LOG("timerfd_create failed. %m\n");
		goto exit;
	}

	retval = pipe2(work_queue->pipe_fd, O_NONBLOCK);
	if (retval < 0) {
		ERROR_LOG("pipe failed. %m\n");
		goto exit1;
	}

	/* add to epoll */
	retval = xio_context_add_ev_handler(
			ctx,
			work_queue->timer_fd,
			XIO_POLLIN,
			xio_delayed_action_handler,
			work_queue);
	if (retval) {
		ERROR_LOG("ev_loop_add_cb failed. %m\n");
		goto exit2;
	}

	/* add to epoll */
	retval = xio_context_add_ev_handler(
			ctx,
			work_queue->pipe_fd[0],
			XIO_POLLIN,
			xio_work_action_handler,
			work_queue);
	if (retval) {
		ERROR_LOG("ev_loop_add_cb failed. %m\n");
		goto exit2;
	}

	return work_queue;

exit2:
	close(work_queue->pipe_fd[0]);
	close(work_queue->pipe_fd[1]);
exit1:
	close(work_queue->timer_fd);
exit:
	ufree(work_queue);
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_workqueue_destroy						     */
/*---------------------------------------------------------------------------*/
int xio_workqueue_destroy(struct xio_workqueue *work_queue)
{
	int retval;

	xio_workqueue_disarm(work_queue);

	retval = xio_context_del_ev_handler(
			work_queue->ctx,
			work_queue->timer_fd);
	if (retval)
		ERROR_LOG("ev_loop_del_cb failed. %m\n");

	retval = xio_context_del_ev_handler(
			work_queue->ctx,
			work_queue->pipe_fd[0]);
	if (retval)
		ERROR_LOG("ev_loop_del_cb failed. %m\n");

	xio_timers_list_close(&work_queue->timers_list);

	close(work_queue->pipe_fd[0]);
	close(work_queue->pipe_fd[1]);
	close(work_queue->timer_fd);
	ufree(work_queue);

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_workqueue_add_delayed_work					     */
/*---------------------------------------------------------------------------*/
int xio_workqueue_add_delayed_work(struct xio_workqueue *work_queue,
			    int msec_duration, void *data,
			    void (*function)(void *data),
			    xio_delayed_work_handle_t *dwork)
{
	int			retval = 0;
	enum timers_list_rc	rc;
	xio_work_handle_t	*work = &dwork->work;


	xio_timers_list_lock(&work_queue->timers_list);

	work->function	= function;
	work->data	= data;
	work->flags	|= XIO_WORK_PENDING;

	rc = xio_timers_list_add_duration(
			&work_queue->timers_list,
			((uint64_t)msec_duration) * 1000000ULL,
			&dwork->timer);
	if (rc == TIMERS_LIST_RC_ERROR) {
		ERROR_LOG("adding to timer failed\n");
		retval = -1;
		goto unlock;
	}

	/* if the recently add timer is now the first in list, rearm */
		/* rearm the timer */
	retval = xio_workqueue_rearm(work_queue);
	if (retval)
		ERROR_LOG("xio_workqueue_rearm failed. %m\n");

unlock:
	xio_timers_list_unlock(&work_queue->timers_list);
	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_workqueue_del_delayed_work					     */
/*---------------------------------------------------------------------------*/
int xio_workqueue_del_delayed_work(struct xio_workqueue *work_queue,
				   xio_delayed_work_handle_t *dwork)
{
	int			retval = 0;
	enum timers_list_rc	rc;

	/* stop the timer */
	xio_workqueue_disarm(work_queue);

	xio_timers_list_lock(&work_queue->timers_list);

	dwork->work.flags &= ~XIO_WORK_PENDING;

	rc = xio_timers_list_del(&work_queue->timers_list, &dwork->timer);
	if (rc == TIMERS_LIST_RC_ERROR) {
		ERROR_LOG("deleting work from queue failed. queue is empty\n");
		goto unlock;
	}
	/* rearm the timer */
	retval = xio_workqueue_rearm(work_queue);
	if (retval)
		ERROR_LOG("xio_workqueue_rearm failed. %m\n");
unlock:
	xio_timers_list_unlock(&work_queue->timers_list);
	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_workqueue_add_work						     */
/*---------------------------------------------------------------------------*/
int xio_workqueue_add_work(struct xio_workqueue *work_queue,
			   void *data,
			   void (*function)(void *data),
			   xio_work_handle_t *work)
{
	uint64_t	exp = uint64_from_ptr(work);
	int		s;

	work->function	= function;
	work->data	= data;
	work->flags	|= XIO_WORK_PENDING;

	s = write(work_queue->pipe_fd[1], &exp, sizeof(exp));
	if (s < 0) {
		ERROR_LOG("failed to write to pipe, %m\n");
		return -1;
	}
	if (s != sizeof(uint64_t)) {
		ERROR_LOG("failed to write to pipe, %m\n");
		return -1;
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_workqueue_del_work						     */
/*---------------------------------------------------------------------------*/
int xio_workqueue_del_work(struct xio_workqueue *work_queue,
			   xio_work_handle_t *work)
{
	if (work->flags & XIO_WORK_PENDING) {
		work->flags &= ~XIO_WORK_PENDING;
		return 0;
	}
	return -1;
}
