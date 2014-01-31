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

#define PRINT_COUNTER		4000

/*---------------------------------------------------------------------------*/
/* process_request							     */
/*---------------------------------------------------------------------------*/
void process_request(struct xio_msg *req)
{
	if (1 /* ++cnt == PRINT_COUNTER */) {
		printf("message: [%"PRIu64"] - %s\n",
				(req->sn + 1), (char *)req->in.header.iov_base);
	}

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
	/* automaticly accept the request */
	printf("new session event. session:%p\n", session);

	xio_accept(session, NULL, 0, NULL, 0);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_request callback							     */
/*---------------------------------------------------------------------------*/
static int on_request(struct xio_session *session,
			struct xio_msg *req,
			int more_in_batch,
			void *cb_user_context)
{
	struct xio_msg *rsp;

	/* process request */
	process_request(req);

	/* attach request to response */
	rsp = calloc(1, sizeof(struct xio_msg));
	rsp->request = req;

	rsp->out.header.iov_base =
		strdup("hello world header response");
	rsp->out.header.iov_len =
		strlen(rsp->out.header.iov_base) + 1;

	xio_send_response(rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* responder send complete callback		      			     */
/*---------------------------------------------------------------------------*/
static int on_msg_send_complete(struct xio_session *session,
				struct xio_msg *rsp,
				void *conn_user_context)
{
	/* pitch it */
	free(rsp->out.header.iov_base);
	free(rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* asynchronous callbacks						     */
/*---------------------------------------------------------------------------*/
struct xio_session_ops  server_ops = {
	.on_session_event		=  on_session_event,
	.on_new_session			=  on_new_session,
	.on_msg_send_complete		=  on_msg_send_complete,
	.on_msg				=  on_request,
	.on_msg_error			=  NULL
};

/*---------------------------------------------------------------------------*/
/* main									     */
/*---------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	struct xio_server	*server;	/* server portal */
	char			url[256];
	struct xio_context	*ctx;

	if (argc < 2) {
		printf("Usage: %s <host> <port>\n", argv[0]);
		exit(1);
	}

	/* initialize library */
	xio_init();

	/* create thread context for the client */
	ctx	= xio_context_create(NULL, 0);

	/* create url to connect to */
	sprintf(url, "rdma://%s:%s", argv[1], argv[2]);
	/* bind a listener server to a portal/url */
	server = xio_bind(ctx, &server_ops, url, NULL, 0, NULL);
	if (server) {
		printf("listen to %s\n", url);
		xio_context_run_loop(ctx, XIO_INFINITE);

		/* normal exit phase */
		fprintf(stdout, "exit signaled\n");

		/* free the server */
		xio_unbind(server);
	}


	/* free the context */
	xio_context_destroy(ctx);

	return 0;
}

