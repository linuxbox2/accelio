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
#include <linux/types.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/version.h>

#include "libxio.h"
#include "xio_observer.h"
#include "xio_common.h"
#include "xio_context.h"

/*---------------------------------------------------------------------------*/
/* defines								     */
/*---------------------------------------------------------------------------*/

#define XIO_EV_LOOP_WAKE	1
#define XIO_EV_LOOP_STOP	(1 << 1)
#define XIO_EV_LOOP_DOWN	(1 << 2)

/*---------------------------------------------------------------------------*/
/* structs								     */
/*---------------------------------------------------------------------------*/

struct xio_ev_loop {
	struct xio_context *ctx;
	unsigned long	flags;
	unsigned long	states;
	union {
		struct {
			union {
				wait_queue_head_t wait;
				struct tasklet_struct tasklet;
			};
		};
		struct workqueue_struct *workqueue;
	};
	/* for thread, tasklet and for stopped workqueue  */
	struct llist_head ev_llist;
};

/*---------------------------------------------------------------------------*/
/* forward declarations							     */
/*---------------------------------------------------------------------------*/

static int priv_ev_loop_run(void *loop_hndl);
static void priv_ev_loop_stop(void *loop_hndl);

static void priv_ev_loop_run_tasklet(unsigned long data);
static void priv_ev_loop_run_work(struct work_struct *work);

static int priv_ev_add_thread(void *loop_hndl, struct xio_ev_data *event);
static int priv_ev_add_tasklet(void *loop_hndl, struct xio_ev_data *event);
static int priv_ev_add_workqueue(void *loop_hndl, struct xio_ev_data *event);

/*---------------------------------------------------------------------------*/
/* xio_ev_loop_init							     */
/*---------------------------------------------------------------------------*/
void *xio_ev_loop_init(unsigned long flags, struct xio_context *ctx)
{
	struct xio_ev_loop	*loop;
	struct xio_loop_ops	*ops;
	char queue_name[64];

	loop = kzalloc(sizeof(struct xio_ev_loop), GFP_KERNEL);
	if (loop == NULL) {
		xio_set_error(ENOMEM);
		ERROR_LOG("kmalloc failed. %m\n");
		goto cleanup0;
	}

	set_bit(XIO_EV_LOOP_STOP, &loop->states);

	init_llist_head(&loop->ev_llist);

	/* use default implementation */
	ops = &ctx->loop_ops;
	ops->ev_loop_run  = priv_ev_loop_run;
	ops->ev_loop_stop = priv_ev_loop_stop;

	switch (flags) {
	case XIO_LOOP_GIVEN_THREAD:
		ops->ev_loop_add_event = priv_ev_add_thread;
		init_waitqueue_head(&loop->wait);
		break;
	case XIO_LOOP_TASKLET:
		ops->ev_loop_add_event = priv_ev_add_tasklet;
		tasklet_init(&loop->tasklet, priv_ev_loop_run_tasklet,
			     (unsigned long)loop);
		break;
	case XIO_LOOP_WORKQUEUE:
		/* temp (also change to single thread) */
		sprintf(queue_name, "xio-%p", loop);
		/* check flags and bw comp */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
		loop->workqueue = create_workqueue(queue_name);
#else
		loop->workqueue = alloc_workqueue(queue_name,
						WQ_MEM_RECLAIM | WQ_HIGHPRI,
						0);
#endif
		if (!loop->workqueue) {
			ERROR_LOG("workqueue create failed.\n");
			goto cleanup1;
		}
		ops->ev_loop_add_event = priv_ev_add_workqueue;
		break;
	default:
		ERROR_LOG("wrong type. %lu\n", flags);
		goto cleanup1;
	}

	loop->flags = flags;
	loop->ctx = ctx;

	return loop;

cleanup1:
	clear_bit(XIO_EV_LOOP_STOP, &loop->states);
	kfree(loop);
cleanup0:
	ERROR_LOG("event loop creation failed.\n");
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_ev_loop_destroy                                                          */
/*---------------------------------------------------------------------------*/
void xio_ev_loop_destroy(void *loop_hndl)
{
	struct xio_ev_loop *loop = (struct xio_ev_loop *)loop_hndl;

	if (loop == NULL)
		return;

	set_bit(XIO_EV_LOOP_DOWN, &loop->states);

	/* CLEAN call unhandled events !!!! */

	switch (loop->flags) {
	case XIO_LOOP_GIVEN_THREAD:
		if (!test_and_set_bit(XIO_EV_LOOP_WAKE, &loop->states)) {
			wake_up_interruptible(&loop->wait);
		}
		break;
	case XIO_LOOP_TASKLET:
		tasklet_kill(&loop->tasklet);
		break;
	case XIO_LOOP_WORKQUEUE:
		flush_workqueue(loop->workqueue);
		destroy_workqueue(loop->workqueue);
		break;
	default:
		break;
	}

	kfree(loop);
}

/*---------------------------------------------------------------------------*/
/* priv_ev_add_thread							     */
/*---------------------------------------------------------------------------*/
static int priv_ev_add_thread(void *loop_hndl, struct xio_ev_data *event)
{
	struct xio_ev_loop *loop = (struct xio_ev_loop *)loop_hndl;

	/* don't add events */
	if (test_bit(XIO_EV_LOOP_DOWN, &loop->states))
		return 0;

	llist_add(&event->ev_llist, &loop->ev_llist);

	/* don't wake up */
	if (test_bit(XIO_EV_LOOP_STOP, &loop->states))
		return 0;

	if (!test_and_set_bit(XIO_EV_LOOP_WAKE, &loop->states)) {
		wake_up_interruptible(&loop->wait);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* priv_ev_add_tasklet							     */
/*---------------------------------------------------------------------------*/
static int priv_ev_add_tasklet(void *loop_hndl, struct xio_ev_data *event)
{
	struct xio_ev_loop *loop = (struct xio_ev_loop *)loop_hndl;

	/* don't add events */
	if (test_bit(XIO_EV_LOOP_DOWN, &loop->states))
		return 0;

	llist_add(&event->ev_llist, &loop->ev_llist);

	/* don't wake up */
	if (test_bit(XIO_EV_LOOP_STOP, &loop->states))
		return 0;

	tasklet_schedule(&loop->tasklet);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* priv_ev_add_workqueue						     */
/*---------------------------------------------------------------------------*/
static int priv_ev_add_workqueue(void *loop_hndl, struct xio_ev_data *event)
{
	struct xio_ev_loop *loop = (struct xio_ev_loop *)loop_hndl;

	/* don't add events */
	if (test_bit(XIO_EV_LOOP_DOWN, &loop->states))
		return 0;

	if (test_bit(XIO_EV_LOOP_STOP, &loop->states)) {
		/* delayed put in link list until resume */
		llist_add(&event->ev_llist, &loop->ev_llist);
		return 0;
	}

	event->work.func = priv_ev_loop_run_work;
	queue_work(loop->workqueue, &event->work);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_ev_loop_run_tasklet						     */
/*---------------------------------------------------------------------------*/
static void priv_ev_loop_run_tasklet(unsigned long data)
{
	struct xio_ev_loop *loop = (struct xio_ev_loop *) data;
	struct xio_ev_data	*tev;
	struct llist_node	*node;

	while (!llist_empty(&loop->ev_llist)) {
		node = llist_del_first(&loop->ev_llist);
		tev = llist_entry(node, struct xio_ev_data, ev_llist);
		tev->handler(tev->data);
	}
}

/*---------------------------------------------------------------------------*/
/* priv_ev_loop_run_work							     */
/*---------------------------------------------------------------------------*/
static void priv_ev_loop_run_work(struct work_struct *work)
{
	struct xio_ev_data *tev = container_of(work, struct xio_ev_data, work);

	tev->handler(tev->data);
}

/*---------------------------------------------------------------------------*/
/* priv_ev_loop_run							     */
/*---------------------------------------------------------------------------*/
int priv_ev_loop_run(void *loop_hndl)
{
	struct xio_ev_loop	*loop = loop_hndl;
	struct xio_ev_data	*tev;
	struct llist_node	*node;

	clear_bit(XIO_EV_LOOP_STOP, &loop->states);

	switch (loop->flags) {
	case XIO_LOOP_GIVEN_THREAD:
		if (loop->ctx->worker != (uint64_t) get_current()) {
			ERROR_LOG("worker kthread(%p) is not current(%p).\n",
				  (void *) loop->ctx->worker, get_current());
			goto cleanup0;
		}
		break;
	case XIO_LOOP_TASKLET:
		/* were events added to list while in STOP state ? */
		if (!llist_empty(&loop->ev_llist))
			tasklet_schedule(&loop->tasklet);
		return 0;
	case XIO_LOOP_WORKQUEUE:
		/* were events added to list while in STOP state ? */
		while (!llist_empty(&loop->ev_llist)) {
			node = llist_del_first(&loop->ev_llist);
			tev = llist_entry(node, struct xio_ev_data, ev_llist);
			tev->work.func = priv_ev_loop_run_work;
			queue_work(loop->workqueue, &tev->work);
		}
		return 0;
	default:
		/* undo */
		set_bit(XIO_EV_LOOP_STOP, &loop->states);
		return -1;
	}

retry_wait:
	wait_event_interruptible(loop->wait,
				 test_bit(XIO_EV_LOOP_WAKE, &loop->states));

retry_dont_wait:

	while (!llist_empty(&loop->ev_llist)) {
		node = llist_del_first(&loop->ev_llist);
		tev = llist_entry(node, struct xio_ev_data, ev_llist);
		tev->handler(tev->data);
	}
	/* "race point" */
	clear_bit(XIO_EV_LOOP_WAKE, &loop->states);

	if (unlikely(test_bit(XIO_EV_LOOP_STOP, &loop->states)))
		return 0;

	/* if a new entry was added while we were at "race point"
	 * than wait event might block forever as condition is false */
	if (llist_empty(&loop->ev_llist))
		goto retry_wait;

	/* race detected */
	if (!test_and_set_bit(XIO_EV_LOOP_WAKE, &loop->states))
		goto retry_dont_wait;

	/* was one wakeup was called */
	goto retry_wait;

cleanup0:
	set_bit(XIO_EV_LOOP_STOP, &loop->states);
	return -1;
}

/*---------------------------------------------------------------------------*/
/* priv_ev_loop_stop                                                        */
/*---------------------------------------------------------------------------*/
void priv_ev_loop_stop(void *loop_hndl)
{
	struct xio_ev_loop *loop = loop_hndl;

	if (loop == NULL)
		return;

	set_bit(XIO_EV_LOOP_STOP, &loop->states);

	switch (loop->flags) {
	case XIO_LOOP_GIVEN_THREAD:
		if (!test_and_set_bit(XIO_EV_LOOP_WAKE, &loop->states)) {
			wake_up_interruptible(&loop->wait);
		}
		break;
	case XIO_LOOP_TASKLET:
		break;
	case XIO_LOOP_WORKQUEUE:
		break;
	default:
		break;
	}
}

int xio_ev_loop_run(struct xio_context *ctx)
{
	return ctx->loop_ops.ev_loop_run(ctx->ev_loop);
}

void xio_ev_loop_stop(struct xio_context *ctx)
{
	ctx->loop_ops.ev_loop_stop(ctx->ev_loop);
}

int xio_ev_loop_add_event(struct xio_context *ctx, struct xio_ev_data *data)
{
	return ctx->loop_ops.ev_loop_add_event(ctx->ev_loop, data);
}
