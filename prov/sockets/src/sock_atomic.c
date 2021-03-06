/*
 * Copyright (c) 2014 Intel Corporation, Inc.  All rights reserved.
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

ssize_t sock_ep_tx_atomic(struct fid_ep *ep, 
			  const struct fi_msg_atomic *msg, 
			  const struct fi_ioc *comparev, void **compare_desc, 
			  size_t compare_count, struct fi_ioc *resultv, 
			  void **result_desc, size_t result_count, uint64_t flags)
{
	int i, ret;
	size_t datatype_sz;
	struct sock_op tx_op;
	union sock_iov tx_iov;
	struct sock_conn *conn;
	struct sock_tx_ctx *tx_ctx;
	uint64_t total_len, src_len, dst_len;
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

	if (msg->iov_count > SOCK_EP_MAX_IOV_LIMIT || 
	    msg->rma_iov_count > SOCK_EP_MAX_IOV_LIMIT)
		return -FI_EINVAL;
	
	if (!tx_ctx->enabled)
		return -FI_EOPBADSTATE;

	if (sock_ep->connected) {
		conn = sock_ep_lookup_conn(sock_ep);
	} else {
		conn = sock_av_lookup_addr(sock_ep, tx_ctx->av, msg->addr);
	}

	if (!conn)
		return -FI_EAGAIN;

	SOCK_EP_SET_TX_OP_FLAGS(flags);
	if (flags & SOCK_USE_OP_FLAGS)
		flags |= tx_ctx->attr.op_flags;

	if (sock_ep_is_write_cq_low(&tx_ctx->comp, flags)) {
		SOCK_LOG_ERROR("CQ size low\n");
		return -FI_EAGAIN;
	}

	if ((flags & FI_TRIGGER) &&
	    (ret = sock_queue_atomic_op(ep, msg, comparev, compare_count,
					resultv, result_count, flags, 
					SOCK_OP_ATOMIC)) != 1) {
		return ret;
	}

	src_len = 0;
	datatype_sz = fi_datatype_size(msg->datatype);
	if (flags & FI_INJECT) {
		for (i=0; i< msg->iov_count; i++) {
			src_len += (msg->msg_iov[i].count * datatype_sz);
		}
		if (src_len > SOCK_EP_MAX_INJECT_SZ) {
			return -FI_EINVAL;
		}
		total_len = src_len;
	} else {
		total_len = msg->iov_count * sizeof(union sock_iov);
	}

	total_len += (sizeof(struct sock_op_send) +
		      (msg->rma_iov_count * sizeof(union sock_iov)) +
		      (result_count * sizeof (union sock_iov)));
	
	sock_tx_ctx_start(tx_ctx);
	if (rbfdavail(&tx_ctx->rbfd) < total_len) {
		ret = -FI_EAGAIN;
		goto err;
	}

	memset(&tx_op, 0, sizeof(tx_op));
	tx_op.op = SOCK_OP_ATOMIC;
	tx_op.dest_iov_len = msg->rma_iov_count;
	tx_op.atomic.op = msg->op;
	tx_op.atomic.datatype = msg->datatype;
	tx_op.atomic.res_iov_len = result_count;
	tx_op.atomic.cmp_iov_len = compare_count;

	if (flags & FI_INJECT)
		tx_op.src_iov_len = src_len;
	else 
		tx_op.src_iov_len = msg->iov_count;

	sock_tx_ctx_write_op_send(tx_ctx, &tx_op, flags, (uintptr_t) msg->context,
			msg->addr, (uintptr_t) msg->msg_iov[0].addr, sock_ep, conn);

	if (flags & FI_REMOTE_CQ_DATA) {
		sock_tx_ctx_write(tx_ctx, &msg->data, sizeof(uint64_t));
	}
	
	src_len = 0;
	if (flags & FI_INJECT) {
		for (i=0; i< msg->iov_count; i++) {
			sock_tx_ctx_write(tx_ctx, msg->msg_iov[i].addr,
					  msg->msg_iov[i].count * datatype_sz);
			src_len += (msg->msg_iov[i].count * datatype_sz);
		}
	} else {
		for (i = 0; i< msg->iov_count; i++) {
			tx_iov.ioc.addr = (uintptr_t) msg->msg_iov[i].addr;
			tx_iov.ioc.count = msg->msg_iov[i].count;
			sock_tx_ctx_write(tx_ctx, &tx_iov, sizeof(tx_iov));
			src_len += (tx_iov.ioc.count * datatype_sz);
		}
	}

	if (src_len > SOCK_EP_MAX_ATOMIC_SZ) {
		ret = -FI_EINVAL;
		goto err;
	}

	dst_len = 0;
	for (i = 0; i< msg->rma_iov_count; i++) {
		tx_iov.ioc.addr = msg->rma_iov[i].addr;
		tx_iov.ioc.key = msg->rma_iov[i].key;
		tx_iov.ioc.count = msg->rma_iov[i].count;
		sock_tx_ctx_write(tx_ctx, &tx_iov, sizeof(tx_iov));
		dst_len += (tx_iov.ioc.count * datatype_sz);
	}
	
	if (msg->iov_count && dst_len != src_len) {
		SOCK_LOG_ERROR("Buffer length mismatch\n");
		ret = -FI_EINVAL;
		goto err;
	} else {
		src_len = dst_len;
	}

	dst_len = 0;
	for (i = 0; i< result_count; i++) {
		tx_iov.ioc.addr = (uintptr_t) resultv[i].addr;
		tx_iov.ioc.count = resultv[i].count;
		sock_tx_ctx_write(tx_ctx, &tx_iov, sizeof(tx_iov));
		dst_len += (tx_iov.ioc.count * datatype_sz);
	}

	if (result_count && (dst_len != src_len)) {
		SOCK_LOG_ERROR("Buffer length mismatch\n");
		ret = -FI_EINVAL;
		goto err;
	}

	dst_len = 0;
	for (i = 0; i< compare_count; i++) {
		tx_iov.ioc.addr = (uintptr_t) comparev[i].addr;
		tx_iov.ioc.count = comparev[i].count;
		sock_tx_ctx_write(tx_ctx, &tx_iov, sizeof(tx_iov));
		dst_len += (tx_iov.ioc.count * datatype_sz);
	}

	if (compare_count && (dst_len != src_len)) {
		SOCK_LOG_ERROR("Buffer length mismatch\n");
		ret = -FI_EINVAL;
		goto err;
	}
	
	sock_tx_ctx_commit(tx_ctx);
	return 0;

err:
	sock_tx_ctx_abort(tx_ctx);
	return ret;
}


static ssize_t sock_ep_atomic_writemsg(struct fid_ep *ep,
			const struct fi_msg_atomic *msg, uint64_t flags)
{
	switch (msg->op) {
	case FI_MIN:
	case FI_MAX: 
	case FI_SUM:
	case FI_PROD:
	case FI_LOR:
	case FI_LAND:
	case FI_BOR:
	case FI_BAND:
	case FI_LXOR: 
	case FI_BXOR:
	case FI_ATOMIC_WRITE:
		break;
	default:
		SOCK_LOG_ERROR("Invalid operation type\n");
		return -FI_EINVAL;
	}

	return sock_ep_tx_atomic(ep, msg, NULL, NULL, 0,
				  NULL, NULL, 0, flags);
}

static ssize_t sock_ep_atomic_write(struct fid_ep *ep,
				     const void *buf, size_t count, void *desc,
				     fi_addr_t dest_addr, uint64_t addr, 
				     uint64_t key, enum fi_datatype datatype, 
				     enum fi_op op, void *context)
{
	struct fi_msg_atomic msg;
	struct fi_ioc msg_iov;
	struct fi_rma_ioc rma_iov;

	msg_iov.addr = (void *)buf;
	msg_iov.count = count;
	msg.msg_iov = &msg_iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;

	rma_iov.addr = addr;
	rma_iov.key = key;
	rma_iov.count = count;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;

	msg.datatype = datatype;
	msg.op = op;
	msg.context = context;
	msg.data = 0;

	return sock_ep_atomic_writemsg(ep, &msg, SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_atomic_writev(struct fid_ep *ep,
			const struct fi_ioc *iov, void **desc, size_t count,
			fi_addr_t dest_addr,
			uint64_t addr, uint64_t key,
			enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_msg_atomic msg;
	struct fi_rma_ioc rma_iov;

	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = dest_addr;

	rma_iov.addr = addr;
	rma_iov.key = key;
	rma_iov.count = count;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;

	msg.datatype = datatype;
	msg.op = op;
	msg.context = context;
	msg.data = 0;

	return sock_ep_atomic_writemsg(ep, &msg, SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_atomic_inject(struct fid_ep *ep, const void *buf, size_t count,
				      fi_addr_t dest_addr, uint64_t addr, uint64_t key,
				      enum fi_datatype datatype, enum fi_op op)
{
	struct fi_msg_atomic msg;
	struct fi_ioc msg_iov;
	struct fi_rma_ioc rma_iov;

	msg_iov.addr = (void *)buf;
	msg_iov.count = count;
	msg.msg_iov = &msg_iov;
	msg.iov_count = 1;
	msg.addr = dest_addr;

	rma_iov.addr = addr;
	rma_iov.key = key;
	rma_iov.count = count;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;

	msg.datatype = datatype;
	msg.op = op;
	msg.data = 0;

	return sock_ep_atomic_writemsg(ep, &msg, FI_INJECT | 
				       SOCK_NO_COMPLETION | SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_atomic_readwritemsg(struct fid_ep *ep, 
					    const struct fi_msg_atomic *msg,
					    struct fi_ioc *resultv, void **result_desc, 
					    size_t result_count, uint64_t flags)
{
	switch (msg->op) {
	case FI_MIN:
	case FI_MAX: 
	case FI_SUM:
	case FI_PROD:
	case FI_LOR:
	case FI_LAND:
	case FI_BOR:
	case FI_BAND:
	case FI_LXOR: 
	case FI_BXOR:
	case FI_ATOMIC_READ:
	case FI_ATOMIC_WRITE:
		break;
	default:
		SOCK_LOG_ERROR("Invalid operation type\n");
		return -FI_EINVAL;
	}

	return sock_ep_tx_atomic(ep, msg, NULL, NULL, 0,
				 resultv, result_desc, result_count, flags);
}

static ssize_t sock_ep_atomic_readwrite(struct fid_ep *ep,
			const void *buf, size_t count, void *desc,
			void *result, void *result_desc,
			fi_addr_t dest_addr,
			uint64_t addr, uint64_t key,
			enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_msg_atomic msg;
	struct fi_ioc msg_iov;
	struct fi_rma_ioc rma_iov;
	struct fi_ioc resultv;
	
	if (!buf && op != FI_ATOMIC_READ)
		return -FI_EINVAL;
	if(op == FI_ATOMIC_READ)
		msg_iov.addr = NULL;
	else
		msg_iov.addr = (void *)buf;

	msg_iov.count = count;
	msg.msg_iov = &msg_iov;

	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;

	rma_iov.addr = addr;
	rma_iov.count = 1;
	rma_iov.key = key;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;
	msg.datatype = datatype;
	msg.op = op;
	msg.context = context;
	
	resultv.addr = result;
	resultv.count = 1;
    
	return sock_ep_atomic_readwritemsg(ep, &msg, 
					    &resultv, &result_desc, 1, SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_atomic_readwritev(struct fid_ep *ep,
			const struct fi_ioc *iov, void **desc, size_t count,
			struct fi_ioc *resultv, void **result_desc, size_t result_count,
			fi_addr_t dest_addr,
			uint64_t addr, uint64_t key,
			enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_msg_atomic msg;
	struct fi_rma_ioc rma_iov;

	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = dest_addr;

	rma_iov.addr = addr;
	rma_iov.count = 1;
	rma_iov.key = key;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;
	msg.datatype = datatype;
	msg.op = op;
	msg.context = context;
	
	return sock_ep_atomic_readwritemsg(ep, &msg, 
					   resultv, result_desc, result_count, 
					   SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_atomic_compwritemsg(struct fid_ep *ep,
			const struct fi_msg_atomic *msg,
			const struct fi_ioc *comparev, void **compare_desc, size_t compare_count,
			struct fi_ioc *resultv, void **result_desc, size_t result_count,
			uint64_t flags)
{
	switch (msg->op) {
	case FI_CSWAP:
	case FI_CSWAP_NE:
	case FI_CSWAP_LE:
	case FI_CSWAP_LT:
	case FI_CSWAP_GE:
	case FI_CSWAP_GT:
	case FI_MSWAP:
		break;
	default:
		SOCK_LOG_ERROR("Invalid operation type\n");
		return -FI_EINVAL;
	}

	return sock_ep_tx_atomic(ep, msg, comparev, compare_desc, compare_count,
				 resultv, result_desc, result_count, flags);
}

static ssize_t sock_ep_atomic_compwrite(struct fid_ep *ep,
			const void *buf, size_t count, void *desc,
			const void *compare, void *compare_desc,
			void *result, void *result_desc,
			fi_addr_t dest_addr,
			uint64_t addr, uint64_t key,
			enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_msg_atomic msg;
	struct fi_ioc msg_iov;
	struct fi_rma_ioc rma_iov;
	struct fi_ioc resultv;
	struct fi_ioc comparev;

	msg_iov.addr = (void *)buf;
	msg_iov.count = count;
	msg.msg_iov = &msg_iov;

	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;

	rma_iov.addr = addr;
	rma_iov.count = 1;
	rma_iov.key = key;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;
	msg.datatype = datatype;
	msg.op = op;
	msg.context = context;
	
	resultv.addr = result;
	resultv.count = 1;
	comparev.addr = (void*)compare;
	comparev.count = 1;

	return sock_ep_atomic_compwritemsg(ep, &msg, &comparev, &compare_desc, 1,
					   &resultv, &result_desc, 1, 
					   SOCK_USE_OP_FLAGS);
}

static ssize_t sock_ep_atomic_compwritev(struct fid_ep *ep,
			const struct fi_ioc *iov, void **desc, size_t count,
			const struct fi_ioc *comparev, void **compare_desc, size_t compare_count,
			struct fi_ioc *resultv, void **result_desc, size_t result_count,
			fi_addr_t dest_addr,
			uint64_t addr, uint64_t key,
			enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct fi_msg_atomic msg;
	struct fi_rma_ioc rma_iov;

	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = dest_addr;

	rma_iov.addr = addr;
	rma_iov.count = 1;
	rma_iov.key = key;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;
	msg.datatype = datatype;
	msg.op = op;
	msg.context = context;
	
	return sock_ep_atomic_compwritemsg(ep, &msg, comparev, compare_desc, 1,
					   resultv, result_desc, 1, 
					   SOCK_USE_OP_FLAGS);
}

static int sock_ep_atomic_valid(struct fid_ep *ep, enum fi_datatype datatype, 
			      enum fi_op op, size_t *count)
{
	size_t datatype_sz;

	switch(datatype){
	case FI_FLOAT:
	case FI_DOUBLE:
	case FI_LONG_DOUBLE:
		if (op == FI_BOR || op == FI_BAND ||
		    op == FI_BXOR || op == FI_MSWAP)
			return -FI_ENOENT;
		break;
		
	case FI_FLOAT_COMPLEX:
	case FI_DOUBLE_COMPLEX:
	case FI_LONG_DOUBLE_COMPLEX:
		return -FI_ENOENT;
	default:
		break;
	}

	datatype_sz = fi_datatype_size(datatype);
	if (datatype_sz == 0)
		return -FI_ENOENT;

	*count = (SOCK_EP_MAX_ATOMIC_SZ/datatype_sz);
	return 0;
}

struct fi_ops_atomic sock_ep_atomic = {
	.size = sizeof(struct fi_ops_atomic),
	.write = sock_ep_atomic_write,
	.writev = sock_ep_atomic_writev,
	.writemsg = sock_ep_atomic_writemsg,
	.inject = sock_ep_atomic_inject,
	.readwrite = sock_ep_atomic_readwrite,
	.readwritev = sock_ep_atomic_readwritev,
	.readwritemsg = sock_ep_atomic_readwritemsg,
	.compwrite = sock_ep_atomic_compwrite,
	.compwritev = sock_ep_atomic_compwritev,
	.compwritemsg = sock_ep_atomic_compwritemsg,
	.writevalid = sock_ep_atomic_valid,
	.readwritevalid = sock_ep_atomic_valid,
	.compwritevalid = sock_ep_atomic_valid,
};
