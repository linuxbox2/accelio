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
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/param.h>
#include <sys/types.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <fcntl.h>
#include "libxio.h"

#include "raio_buffer.h"
#include "raio_command.h"
#include "raio_handlers.h"
#include "raio_utils.h"
#include "raio_bs.h"
#include "libraio.h"
#include "msg_pool.h"

/*---------------------------------------------------------------------------*/
/* preprocessor macros				                             */
/*---------------------------------------------------------------------------*/
#define MAXBLOCKSIZE	(128*1024)
#define BS_IODEPTH	128
#define NULL_BS_DEV_SIZE (1ULL << 32)
#define EXTRA_MSGS	100


/*---------------------------------------------------------------------------*/
/* data structres				                             */
/*---------------------------------------------------------------------------*/
struct raio_io_u {
	struct raio_event		ev_data;
	struct xio_msg			*rsp;
	struct raio_io_cmd		iocmd;

	TAILQ_ENTRY(raio_io_u)		io_u_list;
};

struct raio_io_portal_data {
	struct raio_bs			*bs_dev;
	void				*loop;
	int				iodepth;
	int				io_nr;
	int				io_u_free_nr;
	int				pad;
	struct raio_io_u		*io_us_free;
	struct msg_pool			*rsp_pool;

	struct xio_msg			rsp;
	char				rsp_hdr[512];

	TAILQ_HEAD(, raio_io_u)		io_u_free_list;
};

struct raio_io_session_data {
	int				fd;
	int				is_null;
	int				iothreads;
	int				pad;
	uint64_t			fsize;

	struct raio_io_portal_data	*pd;
};

/*---------------------------------------------------------------------------*/
/* raio_handler_init_session_data				             */
/*---------------------------------------------------------------------------*/
void *raio_handler_init_session_data(int portals_nr)
{
	struct raio_io_session_data *sd;
	sd = calloc(1, sizeof(*sd));

	sd->pd = calloc(portals_nr, sizeof(*sd->pd));

	sd->fd = -1;

	return sd;
}

/*---------------------------------------------------------------------------*/
/* raio_handler_init_portal_data				             */
/*---------------------------------------------------------------------------*/
void *raio_handler_init_portal_data(void *prv_session_data,
				    int portal_nr, void *loop)
{
	struct raio_io_session_data *sd = prv_session_data;
	struct raio_io_portal_data *pd = &sd->pd[portal_nr];

	pd->loop = loop;
	pd->rsp.out.header.iov_base = pd->rsp_hdr;
	pd->rsp.out.header.iov_len  = sizeof(pd->rsp_hdr);

	return pd;
}

/*---------------------------------------------------------------------------*/
/* raio_handler_free_session_data				             */
/*---------------------------------------------------------------------------*/
void raio_handler_free_session_data(void *prv_session_data)
{
	struct raio_io_session_data *sd = prv_session_data;
	free(sd->pd);
	if (sd->fd != -1 && !sd->is_null)
		close(sd->fd);

	free(sd);
}

/*---------------------------------------------------------------------------*/
/* raio_handler_free_portal_data				             */
/*---------------------------------------------------------------------------*/
void raio_handler_free_portal_data(void *prv_portal_data)
{
}

/*---------------------------------------------------------------------------*/
/* raio_handle_open				                             */
/*---------------------------------------------------------------------------*/
static int raio_handle_open(void *prv_session_data,
			    void *prv_portal_data,
			    struct raio_command *cmd,
			    char *cmd_data,
			    struct xio_msg *req)
{
	struct raio_io_session_data	*sd = prv_session_data;
	struct raio_io_portal_data	*pd = prv_portal_data;
	const char	*pathname;
	uint32_t	flags = 0;
	unsigned	overall_size;
	int		fd;
	int		retval;
	struct stat64	stbuf;


	overall_size = sizeof(fd);

	pathname = unpack_u32(&flags,
			      cmd_data);


	if (sizeof(flags) + strlen(pathname) + 1 != cmd->data_len) {
		fd = -1;
		errno = EINVAL;
		printf("open request rejected\n");
		goto reject;
	}

	if (strcmp(pathname, "/dev/null")) {
		fd = open(pathname, flags);
		if (fd == -1)
			goto reject;

	} else {
		sd->is_null = 1;
		fd = 0;
	}

	/* get file size */
	if (sd->is_null) {
		sd->fsize = NULL_BS_DEV_SIZE;
	} else {
		retval = fstat64(fd, &stbuf);
		if (retval == 0) {
			if (S_ISBLK(stbuf.st_mode)) {
				retval = ioctl(fd, BLKGETSIZE64,
					       &stbuf.st_size);
				if (retval < 0)
					fprintf(stderr,
						"Cannot get size, %m\n");
			}
			sd->fsize = stbuf.st_size;
		}
	}


reject:
	 if (fd == -1) {
		 struct raio_answer ans = {RAIO_CMD_OPEN, 0,
					   -1, errno};
		 pack_u32((uint32_t *)&ans.ret_errno,
			  pack_u32((uint32_t *)&ans.ret,
			  pack_u32(&ans.data_len,
			  pack_u32(&ans.command,
			  pd->rsp_hdr))));
		fprintf(stderr, "open %s failed %m\n", pathname);
	 } else {
		 unsigned overall_size = sizeof(fd);
		 struct raio_answer ans = {RAIO_CMD_OPEN,
					   overall_size, 0, 0};
		 pack_u32((uint32_t *)&fd,
			  pack_u32((uint32_t *)&ans.ret_errno,
			  pack_u32((uint32_t *)&ans.ret,
			  pack_u32(&ans.data_len,
			  pack_u32(&ans.command,
			  pd->rsp_hdr)))));
	 }

	pd->rsp.out.header.iov_len = (sizeof(struct raio_answer) +
				     overall_size);

	pd->rsp.request = req;

	xio_send_response(&pd->rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* raio_handle_close				                             */
/*---------------------------------------------------------------------------*/
static int raio_handle_close(void *prv_session_data,
			     void *prv_portal_data,
			     struct raio_command *cmd,
			     char *cmd_data,
			     struct xio_msg *req)
{
	struct raio_io_session_data	*sd = prv_session_data;
	struct raio_io_portal_data	*pd = prv_portal_data;
	int				fd;
	int				retval = 0;

	unpack_u32((uint32_t *)&fd,
		    cmd_data);

	if (sizeof(fd) != cmd->data_len) {
		retval = -1;
		errno = EINVAL;
		printf("open request rejected\n");
		goto reject;
	}

	if (!sd->is_null)
		retval = close(fd);

reject:
	if (retval != 0) {
		struct raio_answer ans = { RAIO_CMD_CLOSE, 0, -1, errno };
		pack_u32((uint32_t *)&ans.ret_errno,
		pack_u32((uint32_t *)&ans.ret,
		pack_u32(&ans.data_len,
		pack_u32(&ans.command,
			 pd->rsp_hdr))));
	} else {
		struct raio_answer ans = { RAIO_CMD_CLOSE, 0, 0, 0 };
		pack_u32((uint32_t *)&ans.ret_errno,
		pack_u32((uint32_t *)&ans.ret,
		pack_u32(&ans.data_len,
		pack_u32(&ans.command,
			 pd->rsp_hdr))));
	 }

	pd->rsp.out.header.iov_len = sizeof(struct raio_answer);
	pd->rsp.out.data_iovlen = 0;

	pd->rsp.request = req;

	xio_send_response(&pd->rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* raio_handle_fstat				                             */
/*---------------------------------------------------------------------------*/
static int raio_handle_fstat(void *prv_session_data,
			     void *prv_portal_data,
			     struct raio_command *cmd,
			     char *cmd_data,
			     struct xio_msg *req)
{
	struct raio_io_session_data	*sd = prv_session_data;
	struct raio_io_portal_data	*pd = prv_portal_data;
	int				fd;
	int				retval = 0;
	struct stat64			stbuf;

	unpack_u32((uint32_t *)&fd,
		    cmd_data);

	if (sizeof(fd) != cmd->data_len) {
		retval = -1;
		errno = EINVAL;
		printf("open request rejected\n");
		goto reject;
	}

	if (sd->is_null) {
		stbuf.st_size = NULL_BS_DEV_SIZE;
		sd->fsize = stbuf.st_size;
	} else {
		retval = fstat64(fd, &stbuf);
		if (retval == 0) {
			if (S_ISBLK(stbuf.st_mode)) {
				retval = ioctl(fd, BLKGETSIZE64,
					       &stbuf.st_size);
				if (retval < 0)
					fprintf(stderr, "Cannot get size %m\n");
			}
			sd->fsize = stbuf.st_size;
		}
	}

reject:
	if (retval != 0) {
		struct raio_answer ans = { RAIO_CMD_FSTAT, 0, -1, errno };
		pack_u32((uint32_t *)&ans.ret_errno,
		pack_u32((uint32_t *)&ans.ret,
		pack_u32(&ans.data_len,
		pack_u32(&ans.command,
			  pd->rsp_hdr))));
	} else {
		struct raio_answer ans = {RAIO_CMD_FSTAT,
					  STAT_BLOCK_SIZE, 0, 0};
		pack_stat64(&stbuf,
		pack_u32((uint32_t *)&ans.ret_errno,
		pack_u32((uint32_t *)&ans.ret,
		pack_u32(&ans.data_len,
		pack_u32(&ans.command,
			 pd->rsp_hdr)))));
	}

	pd->rsp.out.header.iov_len = sizeof(struct raio_answer) +
				     STAT_BLOCK_SIZE;

	pd->rsp.request = req;

	xio_send_response(&pd->rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* raio_handle_setup				                             */
/*---------------------------------------------------------------------------*/
static int raio_handle_setup(void *prv_session_data,
			     void *prv_portal_data,
			     struct raio_command *cmd,
			     char *cmd_data,
			     struct xio_msg *req)
{
	int				fd, j;
	struct raio_io_session_data	*sd = prv_session_data;
	struct raio_io_portal_data	*pd = prv_portal_data;

	if (3*sizeof(int) != cmd->data_len) {
		errno = EINVAL;
		printf("io setup request rejected\n");
		goto reject;
	}

	unpack_u32((uint32_t *)&pd->iodepth,
	unpack_u32((uint32_t *)&fd,
		    cmd_data));

	pd->io_u_free_nr = pd->iodepth + EXTRA_MSGS;
	pd->io_us_free = calloc(pd->io_u_free_nr, sizeof(struct raio_io_u));
	pd->rsp_pool = msg_pool_create(512, MAXBLOCKSIZE, pd->io_u_free_nr);

	TAILQ_INIT(&pd->io_u_free_list);

	/* register each io_u in the free list */
	for (j = 0; j < pd->io_u_free_nr; j++) {
		pd->io_us_free[j].rsp = msg_pool_get(pd->rsp_pool);
		TAILQ_INSERT_TAIL(&pd->io_u_free_list,
				  &pd->io_us_free[j],
				  io_u_list);
	}

	if (sd->is_null)
		pd->bs_dev = raio_bs_init(pd->loop, "null");
	else
		pd->bs_dev = raio_bs_init(pd->loop, "aio");

	errno = -raio_bs_open(pd->bs_dev, fd);

reject:
	if (errno) {
		struct raio_answer ans = { RAIO_CMD_IO_SETUP, 0, -1, errno };
		pack_u32((uint32_t *)&ans.ret_errno,
		pack_u32((uint32_t *)&ans.ret,
		pack_u32(&ans.data_len,
		pack_u32(&ans.command,
			 pd->rsp_hdr))));
	} else {
		struct raio_answer ans = { RAIO_CMD_IO_SETUP, 0, 0, 0 };
		pack_u32((uint32_t *)&ans.ret_errno,
		pack_u32((uint32_t *)&ans.ret,
		pack_u32(&ans.data_len,
		pack_u32(&ans.command,
			pd->rsp_hdr))));
	 }

	pd->rsp.out.header.iov_len = sizeof(struct raio_answer);
	pd->rsp.request = req;

	xio_send_response(&pd->rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* raio_handle_destroy				                             */
/*---------------------------------------------------------------------------*/
static int raio_handle_destroy(void *prv_session_data,
			       void *prv_portal_data,
			       struct raio_command *cmd,
			       char *cmd_data,
			       struct xio_msg *req)
{
	struct raio_io_portal_data	*pd = prv_portal_data;
	int				fd;
	int				retval = 0;

	unpack_u32((uint32_t *)&fd,
		    cmd_data);

	if (sizeof(fd) != cmd->data_len) {
		retval = -1;
		errno = EINVAL;
		printf("open request rejected\n");
		goto reject;
	}

reject:
	if (retval == -1) {
		struct raio_answer ans = { RAIO_CMD_IO_DESTROY, 0, -1, errno };
		pack_u32((uint32_t *)&ans.ret_errno,
		pack_u32((uint32_t *)&ans.ret,
		pack_u32(&ans.data_len,
		pack_u32(&ans.command,
			 pd->rsp_hdr))));
	} else {
		struct raio_answer ans = { RAIO_CMD_IO_DESTROY, 0, 0, 0 };
		pack_u32((uint32_t *)&ans.ret_errno,
		pack_u32((uint32_t *)&ans.ret,
		pack_u32(&ans.data_len,
		pack_u32(&ans.command,
			 pd->rsp_hdr))));
	}

	pd->rsp.out.header.iov_len = sizeof(struct raio_answer);
	pd->rsp.out.data_iovlen = 0;

	pd->rsp.request = req;

	xio_send_response(&pd->rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* raio_reject_request				                             */
/*---------------------------------------------------------------------------*/
int raio_reject_request(void *prv_session_data,
			void *prv_portal_data,
			struct raio_command *cmd,
			char *cmd_data,
			struct xio_msg *req)
{
	struct raio_io_portal_data	*pd = prv_portal_data;

	struct raio_answer ans = { RAIO_CMD_UNKNOWN, 0, -1, errno };
	pack_u32((uint32_t *)&ans.ret_errno,
	pack_u32((uint32_t *)&ans.ret,
	pack_u32(&ans.data_len,
	pack_u32(&ans.command,
		 pd->rsp_hdr))));

	pd->rsp.out.header.iov_len = sizeof(struct raio_answer);
	pd->rsp.out.data_iovlen = 0;
	pd->rsp.request = req;

	xio_send_response(&pd->rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_cmd_submit_comp				                             */
/*---------------------------------------------------------------------------*/
static int on_cmd_submit_comp(struct raio_io_cmd *iocmd)
{
	struct raio_io_u	*io_u = iocmd->user_context;
	struct raio_answer	ans = { RAIO_CMD_IO_SUBMIT, 0, 0, 0 };

	pack_u32((uint32_t *)&iocmd->res2,
	pack_u32((uint32_t *)&iocmd->res,
	pack_u32((uint32_t *)&ans.ret_errno,
	pack_u32((uint32_t *)&ans.ret,
	pack_u32(&ans.data_len,
	pack_u32(&ans.command,
	io_u->rsp->out.header.iov_base))))));

	io_u->rsp->out.header.iov_len = sizeof(struct raio_answer) +
					2*sizeof(uint32_t);

	if (iocmd->res != iocmd->bcount) {
		if (iocmd->res < iocmd->bcount) {
			io_u->rsp->out.data_iov[0].iov_len = iocmd->res;
		} else {
			io_u->rsp->out.data_iovlen	   = 0;
			io_u->rsp->out.data_iov[0].iov_len = iocmd->res;
		}
	} else {
		io_u->rsp->out.data_iov[0].iov_len = iocmd->bcount;
	}

	xio_send_response(io_u->rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* raio_handle_submit				                             */
/*---------------------------------------------------------------------------*/
static int raio_handle_submit(void *prv_session_data,
			      void *prv_portal_data,
			      struct raio_command *cmd,
			      char *cmd_data,
			      struct xio_msg *req)
{
	struct raio_io_portal_data	*pd = prv_portal_data;
	struct raio_io_session_data	*sd = prv_session_data;
	struct raio_io_u		*io_u;
	struct raio_iocb		iocb;
	struct raio_answer		ans;
	int				retval;
	uint32_t			is_last_in_batch;
	uint32_t			msg_sz = SUBMIT_BLOCK_SIZE +
						 sizeof(uint32_t);

	io_u = TAILQ_FIRST(&pd->io_u_free_list);
	if (!io_u) {
		printf("io_u_free_list empty\n");
		errno = ENOSR;
		return -1;
	}

	TAILQ_REMOVE(&pd->io_u_free_list, io_u, io_u_list);
	msg_reset(io_u->rsp);
	pd->io_u_free_nr--;

	if (msg_sz != cmd->data_len) {
		retval = EINVAL;
		printf("io submit request rejected\n");

		goto reject;
	}
	unpack_iocb(&iocb,
	unpack_u32(&is_last_in_batch,
		   cmd_data));

	io_u->iocmd.fd			= iocb.raio_fildes;
	io_u->iocmd.op			= iocb.raio_lio_opcode;
	io_u->iocmd.bcount		= iocb.u.c.nbytes;
	io_u->iocmd.buf			= io_u->rsp->out.data_iov[0].iov_base;
	io_u->iocmd.mr			= io_u->rsp->out.data_iov[0].mr;
	io_u->iocmd.fsize		= sd->fsize;
	io_u->iocmd.offset		= iocb.u.c.offset;
	io_u->iocmd.is_last_in_batch    = is_last_in_batch;
	io_u->iocmd.res			= 0;
	io_u->iocmd.res2		= 0;
	io_u->iocmd.user_context	= io_u;
	io_u->iocmd.comp_cb		= on_cmd_submit_comp;

	io_u->rsp->request		= req;
	io_u->rsp->user_context		= io_u;
	io_u->rsp->out.data_iovlen	= 1;


	/* issues request to bs */
	retval = -raio_bs_cmd_submit(pd->bs_dev, &io_u->iocmd);
	if (retval)
		goto reject;

	return 0;
reject:
	TAILQ_INSERT_TAIL(&pd->io_u_free_list, io_u, io_u_list);
	pd->io_u_free_nr++;
	msg_reset(&pd->rsp);

	ans.command	= RAIO_CMD_IO_SUBMIT;
	ans.data_len	= 0;
	ans.ret		= -1;
	ans.ret_errno	= retval;

	pack_u32((uint32_t *)&ans.ret_errno,
	pack_u32((uint32_t *)&ans.ret,
	pack_u32(&ans.data_len,
	pack_u32(&ans.command,
		 pd->rsp_hdr))));

	pd->rsp.out.header.iov_len = sizeof(struct raio_answer);
	pd->rsp.request = req;

	xio_send_response(&pd->rsp);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* raio_handle_submit_comp				                     */
/*---------------------------------------------------------------------------*/
static int raio_handle_submit_comp(void *prv_session_data,
				   void *prv_portal_data,
				   struct xio_msg *rsp)
{
	struct raio_io_portal_data *pd = prv_portal_data;
	struct raio_io_u	   *io_u = rsp->user_context;

	if (io_u) {
		TAILQ_INSERT_TAIL(&pd->io_u_free_list, io_u, io_u_list);
		pd->io_u_free_nr++;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* raio_handle_destroy_comp				                     */
/*---------------------------------------------------------------------------*/
static int raio_handle_destroy_comp(void *prv_session_data,
				    void *prv_portal_data,
				    struct xio_msg *rsp)
{
	struct raio_io_portal_data *pd = prv_portal_data;
	int			    j;

	raio_bs_close(pd->bs_dev);
	raio_bs_exit(pd->bs_dev);

	for (j = 0; j < pd->iodepth; j++)
		msg_pool_put(pd->rsp_pool, pd->io_us_free[j].rsp);

	TAILQ_INIT(&pd->io_u_free_list);

	free(pd->io_us_free);
	pd->io_us_free = NULL;
	pd->io_u_free_nr = 0;
	msg_pool_delete(pd->rsp_pool);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* raio_handler_on_req				                             */
/*---------------------------------------------------------------------------*/
void raio_handler_on_req(void *prv_session_data,
			 void *prv_portal_data,
			 struct xio_msg *req)
{
	char			*buffer = req->in.header.iov_base;
	char			*cmd_data;
	struct raio_command	cmd;


	if (buffer == NULL) {
		raio_reject_request(prv_session_data,
				    prv_portal_data,
				    &cmd, NULL,
				    req);
		return;
	}

	buffer = (char *)unpack_u32((uint32_t *)&cmd.command,
				    buffer);
	cmd_data = (char *)unpack_u32((uint32_t *)&cmd.data_len,
			      (char *)buffer);

	switch (cmd.command) {
	case RAIO_CMD_IO_SUBMIT:
		raio_handle_submit(prv_session_data,
				   prv_portal_data,
				   &cmd, cmd_data,
				   req);
		break;
	case RAIO_CMD_OPEN:
		raio_handle_open(prv_session_data,
				 prv_portal_data,
				 &cmd, cmd_data,
				 req);
		break;
	case RAIO_CMD_CLOSE:
		raio_handle_close(prv_session_data,
				  prv_portal_data,
				  &cmd, cmd_data,
				  req);
		break;
	case RAIO_CMD_FSTAT:
		raio_handle_fstat(prv_session_data,
				  prv_portal_data,
				  &cmd, cmd_data,
				  req);
		break;
	case RAIO_CMD_IO_SETUP:
		raio_handle_setup(prv_session_data,
				  prv_portal_data,
				  &cmd, cmd_data,
				  req);
		break;
	case RAIO_CMD_IO_DESTROY:
		raio_handle_destroy(prv_session_data,
				    prv_portal_data,
				    &cmd, cmd_data,
				    req);
		break;
	default:
		/*
		printf("unknown command %d len:%d, sn:%"PRIu64"\n",
		       cmd.command, cmd.data_len, req->sn);
		xio_disconnect(conn);
		*/
		raio_reject_request(prv_session_data,
				    prv_portal_data,
				    &cmd, cmd_data,
				    req);
		break;
	};
}

/*---------------------------------------------------------------------------*/
/* raio_handler_on_rsp_comp				                     */
/*---------------------------------------------------------------------------*/
void raio_handler_on_rsp_comp(void *prv_session_data,
			      void *prv_portal_data,
			      struct xio_msg *rsp)
{
	char			*buffer = rsp->out.header.iov_base;
	struct raio_command	cmd;

	unpack_u32(&cmd.command, buffer);

	switch (cmd.command) {
	case RAIO_CMD_IO_SUBMIT:
		raio_handle_submit_comp(prv_session_data,
					prv_portal_data,
					rsp);
		break;
	case RAIO_CMD_IO_DESTROY:
		raio_handle_destroy_comp(prv_session_data,
					 prv_portal_data,
					 rsp);
		break;
	case RAIO_CMD_UNKNOWN:
	case RAIO_CMD_OPEN:
	case RAIO_CMD_CLOSE:
	case RAIO_CMD_FSTAT:
	case RAIO_CMD_IO_SETUP:
		break;
	default:
		printf("unknown answer %d\n", cmd.command);
		break;

	};
}

