/*
 * Copyright (c) 2014-2015 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <limits.h>

#include "sock.h"
#include "sock_util.h"

#define SOCK_LOG_DBG(...) _SOCK_LOG_DBG(FI_LOG_EP_DATA, __VA_ARGS__)
#define SOCK_LOG_ERROR(...) _SOCK_LOG_ERROR(FI_LOG_EP_DATA, __VA_ARGS__)

ssize_t sock_ep_recvmsg(struct fid_ep *ep, const struct fi_msg *msg, 
			uint64_t flags)
{
	int i, ret;
	struct sock_rx_ctx *rx_ctx;
	struct sock_rx_entry *rx_entry;
	struct sock_ep *sock_ep;
	
	switch (ep->fid.fclass) {
	case FI_CLASS_EP:
		sock_ep = container_of(ep, struct sock_ep, ep);
		rx_ctx = sock_ep->rx_ctx;
		break;
	case FI_CLASS_RX_CTX:
	case FI_CLASS_SRX_CTX:
		rx_ctx = container_of(ep, struct sock_rx_ctx, ctx);
		break;
	default:
		SOCK_LOG_ERROR("Invalid ep type\n");
		return -FI_EINVAL;
	}

	if (msg->iov_count > SOCK_EP_MAX_IOV_LIMIT)
		return -FI_EINVAL;

	if (!rx_ctx->enabled)
		return -FI_EOPBADSTATE;

	if (flags & SOCK_USE_OP_FLAGS)
		flags |= rx_ctx->attr.op_flags;

	if (sock_ep_is_recv_cq_low(&rx_ctx->comp, flags)) {
		SOCK_LOG_ERROR("CQ size low\n");
		return -FI_EAGAIN;
	}

	if ((flags & FI_TRIGGER) &&
	    (ret = sock_queue_msg_op(ep, msg, flags, SOCK_OP_RECV)) != 1) {
		return ret;
	}

	if (flags & FI_PEEK) {
		return sock_rx_peek_recv(rx_ctx, msg->addr, 0L, ~0ULL,
					 msg->context, flags, 0);
	} else if (flags & FI_CLAIM) {
		return sock_rx_claim_recv(rx_ctx, msg->context, flags, 
					  0L, ~0ULL, 0, 
					  msg->msg_iov, msg->iov_count);
	}

	rx_entry = sock_rx_new_entry(rx_ctx);
	if (!rx_entry)
		return -FI_ENOMEM;

	rx_entry->rx_op.op = SOCK_OP_RECV;
	rx_entry->rx_op.dest_iov_len = msg->iov_count;

	rx_entry->flags = flags;
	rx_entry->context = (uintptr_t) msg->context;
	rx_entry->addr = (rx_ctx->attr.caps & FI_DIRECTED_RECV) ?
			 msg->addr : FI_ADDR_UNSPEC;
	rx_entry->data = msg->data;
	rx_entry->ignore = ~0ULL;
	rx_entry->is_tagged = 0;

	for (i = 0; i< msg->iov_count; i++) {
		rx_entry->iov[i].iov.addr = (uintptr_t) msg->msg_iov[i].iov_base;
		rx_entry->iov[i].iov.len = msg->msg_iov[i].iov_len;
		rx_entry->total_len += rx_entry->iov[i].iov.len;
	}

	SOCK_LOG_DBG("New rx_entry: %p (ctx: %p)\n", rx_entry, rx_ctx);
	fastlock_acquire(&rx_ctx->lock);
	dlist_insert_tail(&rx_entry->entry, &rx_ctx->rx_entry_list);
	fastlock_release(&rx_ctx->lock);
	return 0;
}

static ssize_t sock_ep_recv(struct fid_ep *ep, void *buf, size_t len, void *desc,
		      fi_addr_t src_addr, void *context)
{
	struct fi_msg msg;
	struct iovec msg_iov;
	memset(&msg, 0, sizeof msg);
	msg_iov.iov_base = buf;
	msg_iov.iov_len = len;

	msg.msg_iov = &msg_iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = src_addr;
	msg.context = context;
	msg.data = 0;
	return sock_ep_recvmsg(ep, &msg, SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_recvv(struct fid_ep *ep, const struct iovec *iov, 
		       void **desc, size_t count, fi_addr_t src_addr, 
		       void *context)
{
	struct fi_msg msg;
	memset(&msg, 0, sizeof msg);
	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = src_addr;
	msg.context = context;
	msg.data = 0;
	return sock_ep_recvmsg(ep, &msg, SOCK_USE_OP_FLAGS);
}

ssize_t sock_ep_sendmsg(struct fid_ep *ep, const struct fi_msg *msg, 
			uint64_t flags)
{
	int ret, i;
	uint64_t total_len;
	struct sock_op tx_op;
	union sock_iov tx_iov;
	struct sock_conn *conn;
	struct sock_tx_ctx *tx_ctx;
	struct sock_ep *sock_ep;

	switch (ep->fid.fclass) {
	case FI_CLASS_EP:
		sock_ep = container_of(ep, struct sock_ep, ep);
		tx_ctx = sock_ep->tx_ctx;
		break;
	case FI_CLASS_TX_CTX:
		tx_ctx = container_of(ep, struct sock_tx_ctx, fid.ctx);
		sock_ep = tx_ctx->ep;
		break;
	default:
		SOCK_LOG_ERROR("Invalid EP type\n");
		return -FI_EINVAL;
	}

	if (msg->iov_count > SOCK_EP_MAX_IOV_LIMIT)
		return -FI_EINVAL;

	if (!tx_ctx->enabled)
		return -FI_EOPBADSTATE;	

	if(sock_drop_packet(sock_ep))
		return 0;

	if (sock_ep->connected) {
		conn = sock_ep_lookup_conn(sock_ep);
	} else {
		conn = sock_av_lookup_addr(sock_ep, tx_ctx->av, msg->addr);
		if (!conn) {
			SOCK_LOG_ERROR("Address lookup failed\n");
			return -errno;
		}
	}
	if (!conn)
		return -FI_EAGAIN;
	
	SOCK_LOG_DBG("New sendmsg on TX: %p using conn: %p\n", 
		      tx_ctx, conn);

	SOCK_EP_SET_TX_OP_FLAGS(flags);
	if (flags & SOCK_USE_OP_FLAGS)
		flags |= tx_ctx->attr.op_flags;

	if (sock_ep_is_send_cq_low(&tx_ctx->comp, flags)) {
		SOCK_LOG_ERROR("CQ size low\n");
		return -FI_EAGAIN;
	}

	if ((flags & FI_TRIGGER) &&
	    (ret = sock_queue_msg_op(ep, msg, flags, SOCK_OP_SEND)) != 1) {
		return ret;
	}

	memset(&tx_op, 0, sizeof(struct sock_op));
	tx_op.op = SOCK_OP_SEND;

	total_len = 0;
	if (flags & FI_INJECT) {
		for (i=0; i< msg->iov_count; i++) {
			total_len += msg->msg_iov[i].iov_len;
		}
		if (total_len > SOCK_EP_MAX_INJECT_SZ) {
			ret = -FI_EINVAL;
			goto err;
		}
		tx_op.src_iov_len = total_len;
	} else {
		tx_op.src_iov_len = msg->iov_count;
		total_len = msg->iov_count * sizeof(union sock_iov);
	}

	total_len += sizeof(struct sock_op_send);
	
	if (flags & FI_REMOTE_CQ_DATA)
		total_len += sizeof(uint64_t);

	sock_tx_ctx_start(tx_ctx);
	if (rbfdavail(&tx_ctx->rbfd) < total_len) {
		ret = -FI_EAGAIN;
		goto err;
	}

	sock_tx_ctx_write_op_send(tx_ctx, &tx_op, flags, (uintptr_t) msg->context,
			msg->addr, (uintptr_t) msg->msg_iov[0].iov_base,
			sock_ep, conn);

	if (flags & FI_REMOTE_CQ_DATA) {
		sock_tx_ctx_write(tx_ctx, &msg->data, sizeof(msg->data));
	}

	if (flags & FI_INJECT) {
		for (i=0; i< msg->iov_count; i++) {
			sock_tx_ctx_write(tx_ctx, msg->msg_iov[i].iov_base,
					  msg->msg_iov[i].iov_len);
		}
	} else {
		for (i=0; i< msg->iov_count; i++) {
			tx_iov.iov.addr = (uintptr_t) msg->msg_iov[i].iov_base;
			tx_iov.iov.len = msg->msg_iov[i].iov_len;
			sock_tx_ctx_write(tx_ctx, &tx_iov, sizeof(tx_iov));
		}
	}

	sock_tx_ctx_commit(tx_ctx);
	return 0;

err:
	sock_tx_ctx_abort(tx_ctx);
	return ret;
}

static ssize_t sock_ep_send(struct fid_ep *ep, const void *buf, size_t len, 
		      void *desc, fi_addr_t dest_addr, void *context)
{
	struct fi_msg msg;
	struct iovec msg_iov;
	memset(&msg, 0, sizeof msg);
	msg_iov.iov_base = (void*)buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.context = context;

	return sock_ep_sendmsg(ep, &msg, SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_sendv(struct fid_ep *ep, const struct iovec *iov, 
		       void **desc, size_t count, fi_addr_t dest_addr, 
		       void *context)
{
	struct fi_msg msg;
	memset(&msg, 0, sizeof msg);
	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = dest_addr;
	msg.context = context;
	return sock_ep_sendmsg(ep, &msg, SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_senddata(struct fid_ep *ep, const void *buf, size_t len, 
			  void *desc, uint64_t data, fi_addr_t dest_addr, 
			  void *context)
{
	struct fi_msg msg;
	struct iovec msg_iov;

	msg_iov.iov_base = (void*)buf;
	msg_iov.iov_len = len;
	
	msg.msg_iov = &msg_iov;
	msg.desc = desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.context = context;
	msg.data = data;

	return sock_ep_sendmsg(ep, &msg, FI_REMOTE_CQ_DATA | SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_inject(struct fid_ep *ep, const void *buf, size_t len, 
			fi_addr_t dest_addr)
{
	struct fi_msg msg;
	struct iovec msg_iov;
	
	memset(&msg, 0, sizeof msg);
	msg_iov.iov_base = (void*)buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;
	msg.iov_count = 1;
	msg.addr = dest_addr;

	return sock_ep_sendmsg(ep, &msg, FI_INJECT | 
			       SOCK_NO_COMPLETION | SOCK_USE_OP_FLAGS);
}

static ssize_t	sock_ep_injectdata(struct fid_ep *ep, const void *buf, size_t len,
			   uint64_t data, fi_addr_t dest_addr)
{
	struct fi_msg msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof msg);
	msg_iov.iov_base = (void*)buf;
	msg_iov.iov_len = len;	
	msg.msg_iov = &msg_iov;

	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.data = data;

	return sock_ep_sendmsg(ep, &msg, FI_REMOTE_CQ_DATA | FI_INJECT | 
			       SOCK_NO_COMPLETION | SOCK_USE_OP_FLAGS);
}

struct fi_ops_msg sock_ep_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = sock_ep_recv,
	.recvv = sock_ep_recvv,
	.recvmsg = sock_ep_recvmsg,
	.send = sock_ep_send,
	.sendv = sock_ep_sendv,
	.sendmsg = sock_ep_sendmsg,
	.inject = sock_ep_inject,
	.senddata = sock_ep_senddata,
	.injectdata = sock_ep_injectdata
};

ssize_t sock_ep_trecvmsg(struct fid_ep *ep, 
			 const struct fi_msg_tagged *msg, uint64_t flags)
{
	int i, ret;
	struct sock_rx_ctx *rx_ctx;
	struct sock_rx_entry *rx_entry;
	struct sock_ep *sock_ep;
	
	switch (ep->fid.fclass) {
	case FI_CLASS_EP:
		sock_ep = container_of(ep, struct sock_ep, ep);
		rx_ctx = sock_ep->rx_ctx;
		break;
	case FI_CLASS_RX_CTX:
	case FI_CLASS_SRX_CTX:
		rx_ctx = container_of(ep, struct sock_rx_ctx, ctx);
		break;
	default:
		SOCK_LOG_ERROR("Invalid ep type\n");
		return -FI_EINVAL;
	}

	if (msg->iov_count > SOCK_EP_MAX_IOV_LIMIT)
		return -FI_EINVAL;

	if (!rx_ctx->enabled)
		return -FI_EOPBADSTATE;

	if (flags & SOCK_USE_OP_FLAGS)
		flags |= rx_ctx->attr.op_flags;
	flags &= ~FI_MULTI_RECV;

	if (sock_ep_is_recv_cq_low(&rx_ctx->comp, flags)) {
		SOCK_LOG_ERROR("CQ size low\n");
		return -FI_EAGAIN;
	}

	if ((flags & FI_TRIGGER) &&
	    (ret = sock_queue_tmsg_op(ep, msg, flags, SOCK_OP_TRECV)) != 1) {
		return ret;
	}

	if (flags & FI_PEEK) {
		return sock_rx_peek_recv(rx_ctx, msg->addr, 
					 msg->tag, msg->ignore,
					 msg->context, flags, 1);
	} else if (flags & FI_CLAIM) {
		return sock_rx_claim_recv(rx_ctx, msg->context, flags, 
					  msg->tag, msg->ignore, 1, 
					  msg->msg_iov, msg->iov_count);
	}

	rx_entry = sock_rx_new_entry(rx_ctx);
	if (!rx_entry)
		return -FI_ENOMEM;

	rx_entry->rx_op.op = SOCK_OP_TRECV;
	rx_entry->rx_op.dest_iov_len = msg->iov_count;

	rx_entry->flags = flags;
	rx_entry->context = (uintptr_t) msg->context;
	rx_entry->addr = (rx_ctx->attr.caps & FI_DIRECTED_RECV) ? 
			 msg->addr : FI_ADDR_UNSPEC;
	rx_entry->data = msg->data;
	rx_entry->tag = msg->tag;
	rx_entry->ignore = msg->ignore;
	rx_entry->is_tagged = 1;

	for (i=0; i< msg->iov_count; i++) {
		rx_entry->iov[i].iov.addr = (uintptr_t) msg->msg_iov[i].iov_base;
		rx_entry->iov[i].iov.len = msg->msg_iov[i].iov_len;
		rx_entry->total_len += rx_entry->iov[i].iov.len;
	}

	fastlock_acquire(&rx_ctx->lock);
	SOCK_LOG_DBG("New rx_entry: %p (ctx: %p)\n", rx_entry, rx_ctx);
	dlist_insert_tail(&rx_entry->entry, &rx_ctx->rx_entry_list);
	fastlock_release(&rx_ctx->lock);
	return 0;
}

static ssize_t sock_ep_trecv(struct fid_ep *ep, void *buf, size_t len, void *desc,
			      fi_addr_t src_addr, uint64_t tag, uint64_t ignore, void *context)
{
	struct fi_msg_tagged msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof msg);
	msg_iov.iov_base = buf;
	msg_iov.iov_len = len;

	msg.msg_iov = &msg_iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = src_addr;
	msg.context = context;
	msg.tag = tag;
	msg.ignore = ignore;
	msg.data = 0;
	return sock_ep_trecvmsg(ep, &msg, SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_trecvv(struct fid_ep *ep, const struct iovec *iov, 
			       void **desc, size_t count, fi_addr_t src_addr, 
			       uint64_t tag, uint64_t ignore, void *context)
{
	struct fi_msg_tagged msg;

	memset(&msg, 0, sizeof msg);
	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = src_addr;
	msg.context = context;
	msg.tag = tag;
	msg.ignore = ignore;
	msg.data = 0;
	return sock_ep_trecvmsg(ep, &msg, SOCK_USE_OP_FLAGS);
}

ssize_t sock_ep_tsendmsg(struct fid_ep *ep, 
			 const struct fi_msg_tagged *msg, uint64_t flags)
{
	int ret, i;
	uint64_t total_len;
	struct sock_op tx_op;
	union sock_iov tx_iov;
	struct sock_conn *conn;
	struct sock_tx_ctx *tx_ctx;
	struct sock_ep *sock_ep;
	
	switch (ep->fid.fclass) {
	case FI_CLASS_EP:
		sock_ep = container_of(ep, struct sock_ep, ep);
		tx_ctx = sock_ep->tx_ctx;
		break;
	case FI_CLASS_TX_CTX:
		tx_ctx = container_of(ep, struct sock_tx_ctx, fid.ctx);
		sock_ep = tx_ctx->ep;
		break;
	default:
		SOCK_LOG_ERROR("Invalid EP type\n");
		return -FI_EINVAL;
	}

	if (msg->iov_count > SOCK_EP_MAX_IOV_LIMIT)
		return -FI_EINVAL;

	if (!tx_ctx->enabled)
		return -FI_EOPBADSTATE;
	
	if(sock_drop_packet(sock_ep))
		return 0;
	
	if (sock_ep->connected) {
		conn = sock_ep_lookup_conn(sock_ep);
	} else {
		conn = sock_av_lookup_addr(sock_ep, tx_ctx->av, msg->addr);
		if (!conn) {
			SOCK_LOG_ERROR("Address lookup failed\n");
			return -errno;
		}
	}
	if (!conn)
		return -FI_EAGAIN;
	
	SOCK_EP_SET_TX_OP_FLAGS(flags);
	if (flags & SOCK_USE_OP_FLAGS)
		flags |= tx_ctx->attr.op_flags;

	if (sock_ep_is_send_cq_low(&tx_ctx->comp, flags)) {
		SOCK_LOG_ERROR("CQ size low\n");
		return -FI_EAGAIN;
	}

	if ((flags & FI_TRIGGER) &&
	    (ret = sock_queue_tmsg_op(ep, msg, flags, SOCK_OP_TSEND)) != 1) {
		return ret;
	}

	memset(&tx_op, 0, sizeof(tx_op));
	tx_op.op = SOCK_OP_TSEND;

	total_len = 0;
	if (flags & FI_INJECT) {
		for (i=0; i< msg->iov_count; i++) {
			total_len += msg->msg_iov[i].iov_len;
		}
		tx_op.src_iov_len = total_len;
		if (total_len > SOCK_EP_MAX_INJECT_SZ) {
			ret = -FI_EINVAL;
			goto err;
		}
	} else {
		total_len = msg->iov_count * sizeof(union sock_iov);
		tx_op.src_iov_len = msg->iov_count;
	}

	total_len += sizeof(struct sock_op_tsend);
	if (flags & FI_REMOTE_CQ_DATA)
		total_len += sizeof(uint64_t);
	
	sock_tx_ctx_start(tx_ctx);
	if (rbfdavail(&tx_ctx->rbfd) < total_len) {
		ret = -FI_EAGAIN;
		goto err;
	}

	sock_tx_ctx_write_op_tsend(tx_ctx, &tx_op, flags, (uintptr_t) msg->context,
			msg->addr, (uintptr_t) msg->msg_iov[0].iov_base,
			sock_ep, conn, msg->tag);

	if (flags & FI_REMOTE_CQ_DATA) {
		sock_tx_ctx_write(tx_ctx, &msg->data, sizeof(msg->data));
	}

	if (flags & FI_INJECT) {
		for (i=0; i< msg->iov_count; i++) {
			sock_tx_ctx_write(tx_ctx, msg->msg_iov[i].iov_base,
					  msg->msg_iov[i].iov_len);
		}
	} else {
		for (i=0; i< msg->iov_count; i++) {
			tx_iov.iov.addr = (uintptr_t) msg->msg_iov[i].iov_base;
			tx_iov.iov.len = msg->msg_iov[i].iov_len;
			sock_tx_ctx_write(tx_ctx, &tx_iov, sizeof(tx_iov));
		}
	}
	
	sock_tx_ctx_commit(tx_ctx);
	return 0;

err:
	sock_tx_ctx_abort(tx_ctx);
	return ret;
}

static ssize_t sock_ep_tsend(struct fid_ep *ep, const void *buf, size_t len, 
			      void *desc, fi_addr_t dest_addr, uint64_t tag, void *context)
{
	struct fi_msg_tagged msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof msg);
	msg_iov.iov_base = (void*)buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.context = context;
	msg.tag = tag;

	return sock_ep_tsendmsg(ep, &msg, SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_tsendv(struct fid_ep *ep, const struct iovec *iov, 
			       void **desc, size_t count, fi_addr_t dest_addr, 
			       uint64_t tag, void *context)
{
	struct fi_msg_tagged msg;

	memset(&msg, 0, sizeof msg);
	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = dest_addr;
	msg.context = context;
	msg.tag = tag;
	return sock_ep_tsendmsg(ep, &msg, SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_tsenddata(struct fid_ep *ep, const void *buf, size_t len,
				  void *desc, uint64_t data, fi_addr_t dest_addr, uint64_t tag, 
				  void *context)
{
	struct fi_msg_tagged msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof msg);
	msg_iov.iov_base = (void*)buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;
	msg.desc = desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.context = context;
	msg.data = data;
	msg.tag = tag;

	return sock_ep_tsendmsg(ep, &msg, FI_REMOTE_CQ_DATA | SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_tinject(struct fid_ep *ep, const void *buf, size_t len,
				fi_addr_t dest_addr, uint64_t tag)
{
	struct fi_msg_tagged msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof msg);
	msg_iov.iov_base = (void*)buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.tag = tag;
	return sock_ep_tsendmsg(ep, &msg, FI_INJECT | 
				SOCK_NO_COMPLETION | SOCK_USE_OP_FLAGS);
}

static ssize_t	sock_ep_tinjectdata(struct fid_ep *ep, const void *buf, size_t len,
				    uint64_t data, fi_addr_t dest_addr, uint64_t tag)
{
	struct fi_msg_tagged msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof msg);
	msg_iov.iov_base = (void*)buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;

	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.data = data;
	msg.tag = tag;

	return sock_ep_tsendmsg(ep, &msg, FI_REMOTE_CQ_DATA | FI_INJECT |
				SOCK_NO_COMPLETION | SOCK_USE_OP_FLAGS);
}


struct fi_ops_tagged sock_ep_tagged = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = sock_ep_trecv,
	.recvv = sock_ep_trecvv,
	.recvmsg = sock_ep_trecvmsg,
	.send = sock_ep_tsend,
	.sendv = sock_ep_tsendv,
	.sendmsg = sock_ep_tsendmsg,
	.inject = sock_ep_tinject,
	.senddata = sock_ep_tsenddata,
	.injectdata = sock_ep_tinjectdata,
};

