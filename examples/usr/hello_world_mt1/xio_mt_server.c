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

#define MAX_THREADS		4
#define QUEUE_DEPTH		512
#define HW_PRINT_COUNTER	1000000

struct portals_vec {
	int			vec_len;
	int			pad;
	const char		*vec[MAX_THREADS];
};

struct hw_thread_data {
	char			portal[64];
	int			affinity;
	int			cnt;
	struct xio_msg		rsp[QUEUE_DEPTH];
	void			*loop;
	pthread_t		thread_id;
};

/* server private data */
struct hw_server_data {
	struct hw_thread_data	tdata[MAX_THREADS];
};

static struct portals_vec *portals_get(struct hw_server_data *server_data,
				const char *uri, void *user_context)
{
	/* fill portals array and return it. */
	int			i;
	struct portals_vec	*portals = calloc(1, sizeof(*portals));
	for (i = 0; i < MAX_THREADS; i++) {
		portals->vec[i] = strdup(server_data->tdata[i].portal);
		portals->vec_len++;
	}

	return portals;
}

static void portals_free(struct portals_vec *portals)
{
	int			i;
	for (i = 0; i < portals->vec_len; i++)
		free((char *)(portals->vec[i]));

	free(portals);
}

/*---------------------------------------------------------------------------*/
/* process_request							     */
/*---------------------------------------------------------------------------*/
static void process_request(struct hw_thread_data *tdata,
			    struct xio_msg *req)
{
	if (++tdata->cnt == HW_PRINT_COUNTER) {
		if (req->in.header.iov_base)
		  ((char *)(req->in.header.iov_base))[req->in.header.iov_len]
			  = 0;
		printf("thread [%d] tid:%p - message: [%"PRIu64"] - %s\n",
				tdata->affinity,
				(void *)pthread_self(),
				(req->sn + 1), (char *)req->in.header.iov_base);
		tdata->cnt = 0;
	}
	req->in.header.iov_base	  = NULL;
	req->in.header.iov_len	  = 0;
	req->in.data_iovlen	  = 0;
}

/*---------------------------------------------------------------------------*/
/* on_request callback							     */
/*---------------------------------------------------------------------------*/
static int on_request(struct xio_session *session,
			struct xio_msg *req,
			int more_in_batch,
			void *cb_user_context)
{
	struct hw_thread_data	*tdata = cb_user_context;
	int i = req->sn % QUEUE_DEPTH;

	/* process request */
	process_request(tdata, req);

	/* attach request to response */
	tdata->rsp[i].request = req;

	xio_send_response(&tdata->rsp[i]);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* asynchronous callbacks						     */
/*---------------------------------------------------------------------------*/
struct xio_session_ops  portal_server_ops = {
	.on_session_event		=  NULL,
	.on_new_session			=  NULL,
	.on_msg_send_complete		=  NULL,
	.on_msg				=  on_request,
	.on_msg_error			=  NULL
};

/*---------------------------------------------------------------------------*/
/* worker thread callback						     */
/*---------------------------------------------------------------------------*/
static void *portal_server_cb(void *data)
{
	struct hw_thread_data	*tdata = data;
	cpu_set_t		cpuset;
	struct xio_context	*ctx;
	struct xio_server	*server;
	char			str[128];
	int			i;

	/* set affinity to thread */

	CPU_ZERO(&cpuset);
	CPU_SET(tdata->affinity, &cpuset);

	pthread_setaffinity_np(tdata->thread_id, sizeof(cpu_set_t), &cpuset);

	/* open default event loop */
	tdata->loop = xio_ev_loop_create();

	/* create thread context for the client */
	ctx = xio_ctx_create(NULL, tdata->loop, 0);

	/* bind a listener server to a portal/url */
	printf("thread [%d] - listen:%s\n", tdata->affinity, tdata->portal);
	server = xio_bind(ctx, &portal_server_ops, tdata->portal, NULL, 0, tdata);
	if (server == NULL)
		goto cleanup;

	sprintf(str,"hello world header response from thread %d",
	        tdata->affinity);
	/* create "hello world" message */
	for (i = 0; i <QUEUE_DEPTH; i++) {
		tdata->rsp[i].out.header.iov_base = strdup(str);
		tdata->rsp[i].out.header.iov_len =
			strlen(tdata->rsp[i].out.header.iov_base);
	}

	/* the default xio supplied main loop */
	xio_ev_loop_run(tdata->loop);

	/* normal exit phase */
	fprintf(stdout, "exit signaled\n");

	/* detach the server */
	xio_unbind(server);

	/* free the message */
	for (i = 0; i <QUEUE_DEPTH; i++)
		free(tdata->rsp[i].out.header.iov_base);

cleanup:
	/* free the context */
	xio_ctx_destroy(ctx);

	/* destroy the default loop */
	xio_ev_loop_destroy(&tdata->loop);

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* on_session_event							     */
/*---------------------------------------------------------------------------*/
static int on_session_event(struct xio_session *session,
		struct xio_session_event_data *event_data,
		void *cb_user_context)
{
	printf("session event: %s. session:%p, connection:%p, reason: %s\n",
	       xio_session_event_str(event_data->event),
	       session, event_data->conn,
	       xio_strerror(event_data->reason));

	switch (event_data->event) {
	case XIO_SESSION_NEW_CONNECTION_EVENT:
		break;
	case XIO_SESSION_CONNECTION_CLOSED_EVENT:
		break;
	case XIO_SESSION_TEARDOWN_EVENT:
		xio_session_destroy(session);
		break;
	default:
		break;
	};

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_new_session							     */
/*---------------------------------------------------------------------------*/
static int on_new_session(struct xio_session *session,
			struct xio_new_session_req *req,
			void *cb_user_context)
{
	struct portals_vec *portals;
	struct hw_server_data *server_data = cb_user_context;

	portals = portals_get(server_data, req->uri, req->user_context);

	/* automatic accept the request */
	xio_accept(session, portals->vec, portals->vec_len, NULL, 0);

	portals_free(portals);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* asynchronous callbacks						     */
/*---------------------------------------------------------------------------*/
struct xio_session_ops  server_ops = {
	.on_session_event		=  on_session_event,
	.on_new_session			=  on_new_session,
	.on_msg_send_complete		=  NULL,
	.on_msg				=  NULL,
	.on_msg_error			=  NULL
};

#if UNBOUND_OUTBOUND_THREAD
static struct outbound_data
{
	/* XXX no xio_server, because we never xio_bind */
	pthread_t thread_id;
	struct xio_context *ctx;
	void *loop;
} outbound;
#endif

static struct inbound_data
{
	pthread_t thread_id;
	struct xio_server *server; /* server portal */
	struct hw_server_data server_data;
	char url[256];
	struct xio_context *ctx;
	int ready;
	pthread_mutex_t mtx;
	pthread_cond_t cv;
	void *loop;
} inbound;

#if UNBOUND_OUTBOUND_THREAD
/* extra ctxt for outbound calls */
static void *outbound_thread(void *data)
{
	xio_ev_loop_run(outbound.loop);
	xio_ctx_destroy(outbound.ctx);
	xio_ev_loop_destroy(&outbound.loop);
	return NULL;
}
#endif

/* inbound acceptor/redirector */
static void *acceptor_thread(void *data)
{
	pthread_mutex_lock(&inbound.mtx);

	/* bind a listener server to a portal/url */
	printf("default bind/listen: %s\n", inbound.url);
	inbound.server = xio_bind(inbound.ctx, &server_ops, inbound.url,
				  NULL, 0, &inbound.server_data);
	if (inbound.server == NULL)
		abort();

	inbound.ready = 1;
	pthread_cond_signal(&inbound.cv);
	pthread_mutex_unlock(&inbound.mtx);

	xio_ev_loop_run(inbound.loop);
	xio_unbind(inbound.server);
	xio_ctx_destroy(inbound.ctx);
	xio_ev_loop_destroy(&inbound.loop);
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* main									     */
/*---------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	int			i;
	uint16_t		port = atoi(argv[2]);

	xio_init();

	/* set up for acceptor bind */
	sprintf(inbound.url, "rdma://%s:%d", argv[1], port);
	pthread_mutex_init(&inbound.mtx, NULL);
	pthread_cond_init(&inbound.cv, NULL);

#if UNBOUND_OUTBOUND_THREAD
	/* start outbound thread */
	outbound.loop = xio_ev_loop_create();
	outbound.ctx = xio_ctx_create(NULL, outbound.loop, 0);
	pthread_create(&outbound.thread_id, NULL, outbound_thread, NULL);
#endif

	/* start acceptor thread */
	inbound.loop = xio_ev_loop_create();
	inbound.ctx = xio_ctx_create(NULL, inbound.loop, 0);
	pthread_create(&inbound.thread_id, NULL, acceptor_thread, NULL);

	/* wait for default ctx init */
	do {
		pthread_mutex_lock(&inbound.mtx);
		pthread_cond_wait(&inbound.cv, &inbound.mtx);
	} while (! inbound.ready);

	/* spawn portals */
	for (i = 0; i < MAX_THREADS; i++) {
		inbound.server_data.tdata[i].affinity = i+1;
		port += 1;
		sprintf(inbound.server_data.tdata[i].portal, "rdma://%s:%d",
			argv[1], port);
		pthread_create(&inbound.server_data.tdata[i].thread_id, NULL,
			       portal_server_cb, &inbound.server_data.tdata[i]);
	}

#if UNBOUND_OUTBOUND_THREAD
	pthread_join(outbound.thread_id, NULL);
#endif
	pthread_join(inbound.thread_id, NULL);

	/* normal exit phase */
	fprintf(stdout, "exit signaled\n");

	/* join the threads */
	for (i = 0; i < MAX_THREADS; i++)
		pthread_join(inbound.server_data.tdata[i].thread_id, NULL);

	return 0;
}

