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
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "libxio.h"

#define QUEUE_DEPTH		512
#define HW_PRINT_COUNTER	4000

/* private session data */
struct hw_session_data {
	void			*loop;
	struct xio_connection	*conn;
};

/*---------------------------------------------------------------------------*/
/* process_response							     */
/*---------------------------------------------------------------------------*/
static void process_response(struct xio_msg *rsp)
{
	static uint64_t cnt;

	if (1 /* ++cnt == HW_PRINT_COUNTER */) {
		printf("message: [%"PRIu64"] - %s\n",
		       (rsp->request->sn + 1), (char *)rsp->in.header.iov_base);
		cnt = 0;
	}
}

/*---------------------------------------------------------------------------*/
/* on_session_event							     */
/*---------------------------------------------------------------------------*/
static int on_session_event(struct xio_session *session,
		struct xio_session_event_data *event_data,
		void *cb_user_context)
{
	struct hw_session_data *session_data = cb_user_context;

	printf("session event: %s. reason: %s\n",
	       xio_session_event_str(event_data->event),
	       xio_strerror(event_data->reason));

	switch (event_data->event) {
	case XIO_SESSION_REJECT_EVENT:
	case XIO_SESSION_CONNECTION_DISCONNECTED_EVENT:
		xio_disconnect(event_data->conn);
		break;
	case XIO_SESSION_TEARDOWN_EVENT:
		xio_session_destroy(session);
		xio_ev_loop_stop(session_data->loop, 0);  /* exit */
		break;
	default:
		break;
	};

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
	struct hw_session_data *session_data = cb_user_context;
	struct xio_msg *msg, *omsg; /* we could just use the retiring msg */

	if(rsp->type != XIO_MSG_TYPE_RSP)
		abort();

	/* process the incoming message */
	process_response(rsp);

	/* resend the message */
	msg = calloc(1, sizeof(struct xio_msg));

	/* steal the header */
	omsg = rsp->request;

#if 0 /* XXXX this doesn't work--the buffer we allocated is gone!?! */
	msg->out.header.iov_base = omsg->out.header.iov_base;
	msg->out.header.iov_len = omsg->out.header.iov_len;
#else
	msg->out.header.iov_base =
		strdup("hello world header request");
	msg->out.header.iov_len =
		strlen(msg->out.header.iov_base) + 1;
#endif

	xio_send_request(session_data->conn, msg);

	/* acknowledge xio that response is no longer needed */
	xio_release_response(rsp);

#if 0
	free(omsg); /* XXXX this segfaults!?! */
#endif

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
	struct xio_session	*session;
	char			url[256];
	struct xio_context	*ctx;
	struct hw_session_data	session_data;
	struct xio_msg          *msg;
	int			i = 0;

	/* client session attributes */
	struct xio_session_attr attr = {
		&ses_ops, /* callbacks structure */
		NULL,	  /* no need to pass the server private data */
		0
	};

	/* initialize library */
	xio_init();

	/* open default event loop */
	session_data.loop = xio_ev_loop_create();

	/* create thread context for the client */
	ctx = xio_ctx_create(NULL, session_data.loop, 0);

	/* create url to connect to */
	sprintf(url, "rdma://%s:%s", argv[1], argv[2]);
	session = xio_session_create(XIO_SESSION_CLIENT,
				     &attr, url, 0, 0, &session_data);

	/* connect the session  */
	session_data.conn = xio_connect(session, ctx, 0, NULL, &session_data);

	/* create and send initial "hello world" messages */
	for (i = 0; i < QUEUE_DEPTH; i++) {
		msg = calloc(1, sizeof(struct xio_msg));
		msg->out.header.iov_base =
			strdup("hello world header request");
		msg->out.header.iov_len =
			strlen(msg->out.header.iov_base) + 1;
		xio_send_request(session_data.conn, msg);
	}

	/* the default xio supplied main loop */
	xio_ev_loop_run(session_data.loop);

	/* normal exit phase */
	fprintf(stdout, "exit signaled\n");

	/* free the context */
	xio_ctx_destroy(ctx);

	/* destroy the default loop */
	xio_ev_loop_destroy(&session_data.loop);

	printf("good bye\n");
	return 0;
}

