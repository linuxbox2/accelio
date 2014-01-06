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
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "libxio.h"

//#define PRINT_COUNTER	400000
#define PRINT_COUNTER	1

struct portal_data {
	int			affinity;
	int			pad;
	uint64_t		cnt;
	struct xio_session	*session;
	struct xio_connection	*conn;
	struct xio_context	*ctx;
	struct xio_msg		req;
	void			*loop;
	pthread_t		thread_id;
};


/* private session data */
struct session_data {
	struct xio_session	*session;
	struct portal_data	portal;
};

static void *portal_worker_thread(void *data)
{
	struct portal_data	*portal = data;
	cpu_set_t		cpuset;

	/* set affinity to thread */
	CPU_ZERO(&cpuset);
	CPU_SET(portal->affinity, &cpuset);

	pthread_setaffinity_np(portal->thread_id, sizeof(cpu_set_t), &cpuset);

	/* the default xio supplied main loop */
	xio_ev_loop_run(portal->loop);

	/* normal exit phase */
	fprintf(stdout, "exit signaled\n");

	/* free the context */
	xio_ctx_destroy(portal->ctx);

	/* destroy the default loop */
	xio_ev_loop_destroy(&portal->loop);

	fprintf(stdout, "thread exit\n");
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* process_response							     */
/*---------------------------------------------------------------------------*/
static void process_response(struct portal_data  *portal,
			     struct xio_msg *rsp)
{
	if (++portal->cnt == PRINT_COUNTER) {
		((char *)(rsp->in.header.iov_base))[rsp->in.header.iov_len] = 0;
		printf("thread [%d] - tid:%p  - message: [%"PRIu64"] - %s\n",
		       portal->affinity,
		      (void *)pthread_self(),
		       (rsp->request->sn + 1), (char *)rsp->in.header.iov_base);
		portal->cnt = 0;
	}
	rsp->in.header.iov_base	  = NULL;
	rsp->in.header.iov_len	  = 0;
	rsp->in.data_iovlen	  = 0;
}

/*---------------------------------------------------------------------------*/
/* on_session_event							     */
/*---------------------------------------------------------------------------*/
static int on_session_event(struct xio_session *session,
		struct xio_session_event_data *event_data,
		void *cb_user_context)
{
	struct session_data *session_data = cb_user_context;

	printf("%s. reason: %s\n",
	       xio_session_event_str(event_data->event),
	       xio_strerror(event_data->reason));

	switch (event_data->event) {
	case XIO_SESSION_REJECT_EVENT:
	case XIO_SESSION_CONNECTION_DISCONNECTED_EVENT:
		xio_disconnect(event_data->conn);
		break;
	case XIO_SESSION_TEARDOWN_EVENT:
		xio_ev_loop_stop(session_data->portal.loop, 0);
		break;
	default:
		break;
	};

	/* normal exit phase */
	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_response								     */
/*---------------------------------------------------------------------------*/
static int on_response(struct xio_session *session,
			struct xio_msg *rsp,
			int more_in_batch,
			void *cb_user_context)
{
	struct portal_data  *portal = cb_user_context;

	/* process the incoming message */
	process_response(portal, rsp);

	/* acknowlege xio that response is no longer needed */
	xio_release_response(rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* callbacks								     */
/*---------------------------------------------------------------------------*/
struct xio_session_ops ses_ops = {
	.on_session_event		=  on_session_event,
	.on_session_established		=  NULL,
	.on_msg				=  on_response,
	.on_msg_error			=  NULL
};

/*---------------------------------------------------------------------------*/
/* main									     */
/*---------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	char			url[256];
	struct session_data	sdata;
	char			str[128];


	/* client session attributes */
	struct xio_session_attr attr = {
		&ses_ops, /* callbacks structure */
		NULL,	  /* no need to pass the server private data */
		0
	};
	struct timespec ts = {
		.tv_sec = 1,
		.tv_nsec = 0
	};

	xio_init();

	memset(&sdata, 0, sizeof(sdata));

	/* open default event loop */
	sdata.portal.loop = xio_ev_loop_create();

	/* create thread context for the client */
	sdata.portal.ctx = xio_ctx_create(NULL, sdata.portal.loop, 0);

	/* spawn thread to handle connection */
	sdata.portal.affinity = 1;
	sdata.portal.cnt = 0;

	/* thread is working on the same session */
	sdata.portal.session = sdata.session;
	pthread_create(&sdata.portal.thread_id, NULL,
		       portal_worker_thread, &sdata.portal);

	/* create url to connect to */
	sprintf(url, "rdma://%s:%s", argv[1], argv[2]);
	sdata.session = xio_session_create(XIO_SESSION_REQ,
					   &attr, url, 0, 0, &sdata);

	if (sdata.session == NULL)
		goto cleanup;

	/* connect the session  */
	sdata.portal.conn = xio_connect(
		sdata.session,
		sdata.portal.ctx,
		0, NULL, &sdata.portal);

	/* create "hello world" message */
	memset(&sdata.portal.req, 0, sizeof(sdata.portal.req));
	sprintf(str,"hello world header request from thread %d",
		sdata.portal.affinity);
	sdata.portal.req.out.header.iov_base = strdup(str);
	sdata.portal.req.out.header.iov_len =
		strlen(sdata.portal.req.out.header.iov_base);

	/* send first message */
	while (1) {
		xio_send_request(sdata.portal.conn, &sdata.portal.req);
		nanosleep(&ts, NULL);
	}

	/* join the thread */
	pthread_join(sdata.portal.thread_id, NULL);

	/* close the session */
	xio_session_destroy(sdata.session);

cleanup:

	return 0;
}

