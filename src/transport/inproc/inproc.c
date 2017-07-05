//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "core/nng_impl.h"

// Inproc transport.  This just transports messages from one
// peer to another.  The inproc transport is only valid within the same
// process.

typedef struct nni_inproc_pair		nni_inproc_pair;
typedef struct nni_inproc_pipe		nni_inproc_pipe;
typedef struct nni_inproc_ep		nni_inproc_ep;

typedef struct {
	nni_mtx		mx;
	nni_list	servers;
} nni_inproc_global;

// nni_inproc_pipe represents one half of a connection.
struct nni_inproc_pipe {
	const char *		addr;
	nni_inproc_pair *	pair;
	nni_msgq *		rq;
	nni_msgq *		wq;
	uint16_t		peer;
	uint16_t		proto;
};

// nni_inproc_pair represents a pair of pipes.  Because we control both
// sides of the pipes, we can allocate and free this in one structure.
struct nni_inproc_pair {
	nni_mtx			mx;
	int			refcnt;
	nni_msgq *		q[2];
	nni_inproc_pipe *	pipes[2];
};

struct nni_inproc_ep {
	char		addr[NNG_MAXADDRLEN+1];
	int		mode;
	int		closed;
	nni_list_node	node;
	uint16_t	proto;
	nni_cv		cv;
	nni_list	clients;
	nni_list	aios;
};

#define NNI_INPROC_EP_IDLE	0
#define NNI_INPROC_EP_DIAL	1
#define NNI_INPROC_EP_LISTEN	2

// nni_inproc is our global state - this contains the list of active endpoints
// which we use for coordinating rendezvous.
static nni_inproc_global nni_inproc;

static int
nni_inproc_init(void)
{
	int rv;

	NNI_LIST_INIT(&nni_inproc.servers, nni_inproc_ep, node);

	if ((rv = nni_mtx_init(&nni_inproc.mx)) != 0) {
		return (rv);
	}

	return (0);
}


static void
nni_inproc_fini(void)
{
	nni_mtx_fini(&nni_inproc.mx);
}


static void
nni_inproc_pipe_close(void *arg)
{
	nni_inproc_pipe *pipe = arg;

	if (pipe->rq != NULL) {
		nni_msgq_close(pipe->rq);
	}
	if (pipe->wq != NULL) {
		nni_msgq_close(pipe->wq);
	}
}


// nni_inproc_pair destroy is called when both pipe-ends of the pipe
// have been destroyed.
static void
nni_inproc_pair_destroy(nni_inproc_pair *pair)
{
	nni_msgq_fini(pair->q[0]);
	nni_msgq_fini(pair->q[1]);
	nni_mtx_fini(&pair->mx);
	NNI_FREE_STRUCT(pair);
}


static int
nni_inproc_pipe_init(nni_inproc_pipe **pipep, nni_inproc_ep *ep)
{
	nni_inproc_pipe *pipe;

	if ((pipe = NNI_ALLOC_STRUCT(pipe)) == NULL) {
		return (NNG_ENOMEM);
	}
	pipe->proto = ep->proto;
	pipe->addr = ep->addr;
	*pipep = pipe;
	return (0);
}


static void
nni_inproc_pipe_fini(void *arg)
{
	nni_inproc_pipe *pipe = arg;
	nni_inproc_pair *pair;

	if ((pair = pipe->pair) != NULL) {
		// If we are the last peer, then toss the pair structure.
		nni_mtx_lock(&pair->mx);
		if (pair->pipes[0] == pipe) {
			pair->pipes[0] = NULL;
		} else if (pair->pipes[1] == pipe) {
			pair->pipes[1] = NULL;
		}
		pair->refcnt--;
		if (pair->refcnt == 0) {
			nni_mtx_unlock(&pair->mx);
			nni_inproc_pair_destroy(pair);
		} else {
			nni_mtx_unlock(&pair->mx);
		}
	}

	NNI_FREE_STRUCT(pipe);
}


static void
nni_inproc_pipe_send(void *arg, nni_aio *aio)
{
	nni_inproc_pipe *pipe = arg;
	nni_msg *msg = aio->a_msg;
	char *h;
	size_t l;
	int rv;

	// We need to move any header data to the body, because the other
	// side won't know what to do otherwise.
	h = nni_msg_header(msg);
	l = nni_msg_header_len(msg);
	if ((rv = nni_msg_prepend(msg, h, l)) != 0) {
		nni_aio_finish(aio, rv, aio->a_count);
		return;
	}
	nni_msg_trunc_header(msg, l);
	nni_msgq_aio_put(pipe->wq, aio);
}


static void
nni_inproc_pipe_recv(void *arg, nni_aio *aio)
{
	nni_inproc_pipe *pipe = arg;

	nni_msgq_aio_get(pipe->rq, aio);
}


static uint16_t
nni_inproc_pipe_peer(void *arg)
{
	nni_inproc_pipe *pipe = arg;

	return (pipe->peer);
}


static int
nni_inproc_pipe_getopt(void *arg, int option, void *buf, size_t *szp)
{
	nni_inproc_pipe *pipe = arg;
	size_t len;

	switch (option) {
	case NNG_OPT_LOCALADDR:
	case NNG_OPT_REMOTEADDR:
		len = strlen(pipe->addr) + 1;
		if (len > *szp) {
			(void) memcpy(buf, pipe->addr, *szp);
		} else {
			(void) memcpy(buf, pipe->addr, len);
		}
		*szp = len;
		return (0);
	}
	return (NNG_ENOTSUP);
}


static int
nni_inproc_ep_init(void **epp, const char *url, nni_sock *sock)
{
	nni_inproc_ep *ep;
	int rv;

	if (strlen(url) > NNG_MAXADDRLEN-1) {
		return (NNG_EINVAL);
	}
	if ((ep = NNI_ALLOC_STRUCT(ep)) == NULL) {
		return (NNG_ENOMEM);
	}

	ep->mode = NNI_INPROC_EP_IDLE;
	ep->closed = 0;
	ep->proto = nni_sock_proto(sock);
	NNI_LIST_INIT(&ep->clients, nni_inproc_ep, node);
	nni_aio_list_init(&ep->aios);

	(void) snprintf(ep->addr, sizeof (ep->addr), "%s", url);
	*epp = ep;
	return (0);
}


static void
nni_inproc_ep_fini(void *arg)
{
	nni_inproc_ep *ep = arg;

	NNI_ASSERT(ep->closed);
	NNI_FREE_STRUCT(ep);
}


static void
nni_inproc_conn_finish(nni_aio *aio, int rv)
{
	nni_inproc_ep *ep = aio->a_endpt;

	if (rv != 0) {
		if (aio->a_pipe != NULL) {
			nni_inproc_pipe_fini(aio->a_pipe);
			aio->a_pipe = NULL;
		}
	}
	nni_aio_list_remove(aio);
	if (ep != NULL) {
		if ((ep->mode != NNI_INPROC_EP_LISTEN) &&
		    nni_list_empty(&ep->aios)) {
			if (nni_list_active(&ep->clients, ep)) {
				nni_list_remove(&ep->clients, ep);
			}
		}
	}
	nni_aio_finish(aio, rv, 0);
}


static void
nni_inproc_ep_close(void *arg)
{
	nni_inproc_ep *ep = arg;
	nni_inproc_ep *client;
	nni_aio *aio;

	nni_mtx_lock(&nni_inproc.mx);
	ep->closed = 1;
	if (nni_list_active(&nni_inproc.servers, ep)) {
		nni_list_remove(&nni_inproc.servers, ep);
	}
	// Notify any waiting clients that we are closed.
	while ((client = nni_list_first(&ep->clients)) != NULL) {
		while ((aio = nni_list_first(&client->aios)) != NULL) {
			nni_inproc_conn_finish(aio, NNG_ECONNREFUSED);
		}
		nni_list_remove(&ep->clients, client);
	}
	while ((aio = nni_list_first(&ep->aios)) != NULL) {
		nni_inproc_conn_finish(aio, NNG_ECLOSED);
	}
	nni_mtx_unlock(&nni_inproc.mx);
}


static void
nni_inproc_connect_abort(nni_aio *aio)
{
	nni_inproc_ep *ep = aio->a_endpt;

	nni_mtx_lock(&nni_inproc.mx);

	if (aio->a_pipe != NULL) {
		nni_inproc_pipe_fini(aio->a_pipe);
		aio->a_pipe = NULL;
	}
	nni_aio_list_remove(aio);
	if (ep != NULL) {
		if ((ep->mode != NNI_INPROC_EP_LISTEN) &&
		    nni_list_empty(&ep->aios)) {
			if (nni_list_active(&ep->clients, ep)) {
				nni_list_remove(&ep->clients, ep);
			}
		}
	}
	nni_mtx_unlock(&nni_inproc.mx);
}


static void
nni_inproc_accept_clients(nni_inproc_ep *server)
{
	nni_inproc_ep *client, *nclient;
	nni_aio *saio, *caio;
	nni_inproc_pair *pair;
	int rv;

	nclient = nni_list_first(&server->clients);
	while ((client = nclient) != NULL) {
		nclient = nni_list_next(&server->clients, nclient);
		NNI_LIST_FOREACH (&client->aios, caio) {
			if ((saio = nni_list_first(&server->aios)) == NULL) {
				// No outstanding accept() calls.
				break;
			}

			if ((pair = NNI_ALLOC_STRUCT(pair)) == NULL) {
				nni_inproc_conn_finish(caio, NNG_ENOMEM);
				nni_inproc_conn_finish(saio, NNG_ENOMEM);
				continue;
			}

			if (((rv = nni_mtx_init(&pair->mx)) != 0) ||
			    ((rv = nni_msgq_init(&pair->q[0], 4)) != 0) ||
			    ((rv = nni_msgq_init(&pair->q[1], 4)) != 0)) {
				nni_inproc_pair_destroy(pair);
				nni_inproc_conn_finish(caio, rv);
				nni_inproc_conn_finish(saio, rv);
				continue;
			}

			pair->pipes[0] = caio->a_pipe;
			pair->pipes[1] = saio->a_pipe;
			pair->pipes[0]->rq = pair->pipes[1]->wq = pair->q[0];
			pair->pipes[1]->rq = pair->pipes[0]->wq = pair->q[1];
			pair->pipes[0]->pair = pair->pipes[1]->pair = pair;
			pair->pipes[1]->peer = pair->pipes[0]->proto;
			pair->pipes[0]->peer = pair->pipes[1]->proto;
			pair->refcnt = 2;
			client->mode = NNI_INPROC_EP_IDLE;      // XXX: ??

			nni_inproc_conn_finish(caio, 0);
			nni_inproc_conn_finish(saio, 0);
		}

		if (nni_list_first(&client->aios) == NULL) {
			// No more outstanding client connects.  Normally
			// there should only be one.
			if (nni_list_active(&server->clients, client)) {
				nni_list_remove(&server->clients, client);
			}
		}
	}
}


static void
nni_inproc_ep_connect(void *arg, nni_aio *aio)
{
	nni_inproc_ep *ep = arg;
	nni_inproc_pipe *pipe;
	nni_inproc_ep *server;
	int rv;

	if (ep->mode != NNI_INPROC_EP_IDLE) {
		nni_aio_finish(aio, NNG_EINVAL, 0);
		return;
	}
	if (nni_list_active(&ep->clients, ep)) {
		nni_aio_finish(aio, NNG_EINVAL, 0);
		return;
	}

	if ((rv = nni_inproc_pipe_init(&pipe, ep)) != 0) {
		nni_aio_finish(aio, rv, 0);
		return;
	}

	nni_mtx_lock(&nni_inproc.mx);
	aio->a_pipe = pipe;

	if (nni_list_active(&ep->clients, ep)) {
		// We already have a pending connection...
		nni_inproc_conn_finish(aio, NNG_EINVAL);
		return;
	}

	if (ep->closed) {
		nni_inproc_conn_finish(aio, rv);
		nni_mtx_unlock(&nni_inproc.mx);
		return;
	}

	// Find a server.
	NNI_LIST_FOREACH (&nni_inproc.servers, server) {
		if (server->mode != NNI_INPROC_EP_LISTEN) {
			continue;
		}
		if (strcmp(server->addr, ep->addr) == 0) {
			break;
		}
	}
	if (server == NULL) {
		nni_inproc_conn_finish(aio, NNG_ECONNREFUSED);
		nni_mtx_unlock(&nni_inproc.mx);
		return;
	}

	ep->mode = NNI_INPROC_EP_DIAL;
	nni_list_append(&server->clients, ep);
	nni_aio_list_append(&ep->aios, aio);

	nni_inproc_accept_clients(server);
	nni_mtx_unlock(&nni_inproc.mx);
}


static int
nni_inproc_ep_bind(void *arg)
{
	nni_inproc_ep *ep = arg;
	nni_inproc_ep *srch;
	nni_list *list = &nni_inproc.servers;

	if (ep->mode != NNI_INPROC_EP_IDLE) {
		return (NNG_EINVAL);
	}
	nni_mtx_lock(&nni_inproc.mx);
	if (ep->closed) {
		nni_mtx_unlock(&nni_inproc.mx);
		return (NNG_ECLOSED);
	}
	NNI_LIST_FOREACH (list, srch) {
		if (srch->mode != NNI_INPROC_EP_LISTEN) {
			continue;
		}
		if (strcmp(srch->addr, ep->addr) == 0) {
			nni_mtx_unlock(&nni_inproc.mx);
			return (NNG_EADDRINUSE);
		}
	}
	ep->mode = NNI_INPROC_EP_LISTEN;
	nni_list_append(list, ep);
	nni_mtx_unlock(&nni_inproc.mx);
	return (0);
}


static void
nni_inproc_ep_accept(void *arg, nni_aio *aio)
{
	nni_inproc_ep *ep = arg;
	nni_inproc_pipe *pipe;
	int rv;

	if (ep->mode != NNI_INPROC_EP_LISTEN) {
		nni_aio_finish(aio, NNG_EINVAL, 0);
	}

	if ((rv = nni_inproc_pipe_init(&pipe, ep)) != 0) {
		nni_aio_finish(aio, rv, 0);
		return;
	}

	nni_mtx_lock(&nni_inproc.mx);
	aio->a_pipe = pipe;

	// We are already on the master list of servers, thanks to bind.

	if (ep->closed) {
		// This is the only possible error path from the
		// time we acquired the lock.
		nni_inproc_conn_finish(aio, NNG_ECLOSED);
		return;
	}

	// Insert us into the pending server aios, and then run the
	// accept list.
	nni_aio_list_append(&ep->aios, aio);
	nni_inproc_accept_clients(ep);
	nni_mtx_unlock(&nni_inproc.mx);
}


static nni_tran_pipe nni_inproc_pipe_ops = {
	.p_fini		= nni_inproc_pipe_fini,
	.p_send		= nni_inproc_pipe_send,
	.p_recv		= nni_inproc_pipe_recv,
	.p_close	= nni_inproc_pipe_close,
	.p_peer		= nni_inproc_pipe_peer,
	.p_getopt	= nni_inproc_pipe_getopt,
};

static nni_tran_ep nni_inproc_ep_ops = {
	.ep_init	= nni_inproc_ep_init,
	.ep_fini	= nni_inproc_ep_fini,
	.ep_connect	= nni_inproc_ep_connect,
	.ep_bind	= nni_inproc_ep_bind,
	.ep_accept	= nni_inproc_ep_accept,
	.ep_close	= nni_inproc_ep_close,
	.ep_setopt	= NULL,
	.ep_getopt	= NULL,
};

// This is the inproc transport linkage, and should be the only global
// symbol in this entire file.
struct nni_tran nni_inproc_tran = {
	.tran_scheme	= "inproc",
	.tran_ep	= &nni_inproc_ep_ops,
	.tran_pipe	= &nni_inproc_pipe_ops,
	.tran_init	= nni_inproc_init,
	.tran_fini	= nni_inproc_fini,
};
