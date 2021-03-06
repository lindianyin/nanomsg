/*
    Copyright (c) 2012-2013 250bpm s.r.o.  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
    Copyright (c) 2014 Wirebird Labs LLC.  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "ws.h"
#include "bws.h"
#include "cws.h"
#include "sws.h"

#include "../../ws.h"

#include "../utils/port.h"
#include "../utils/iface.h"

#include "../../utils/err.h"
#include "../../utils/alloc.h"
#include "../../utils/fast.h"
#include "../../utils/list.h"
#include "../../utils/cont.h"

#include <string.h>

#if defined NN_HAVE_WINDOWS
#include "../../utils/win.h"
#else
#include <unistd.h>
#endif

/*  WebSocket-specific socket options. */
struct nn_ws_optset {
    struct nn_optset base;
    int placeholder;
};

static void nn_ws_optset_destroy (struct nn_optset *self);
static int nn_ws_optset_setopt (struct nn_optset *self, int option,
    const void *optval, size_t optvallen);
static int nn_ws_optset_getopt (struct nn_optset *self, int option,
    void *optval, size_t *optvallen);
static const struct nn_optset_vfptr nn_ws_optset_vfptr = {
    nn_ws_optset_destroy,
    nn_ws_optset_setopt,
    nn_ws_optset_getopt
};

/*  nn_transport interface. */
static int nn_ws_bind (void *hint, struct nn_epbase **epbase);
static int nn_ws_connect (void *hint, struct nn_epbase **epbase);
static struct nn_optset *nn_ws_optset (void);

static struct nn_transport nn_ws_vfptr = {
    "ws",
    NN_WS,
    NULL,
    NULL,
    nn_ws_bind,
    nn_ws_connect,
    nn_ws_optset,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_transport *nn_ws = &nn_ws_vfptr;

static int nn_ws_bind (void *hint, struct nn_epbase **epbase)
{
    return nn_bws_create (hint, epbase);
}

static int nn_ws_connect (void *hint, struct nn_epbase **epbase)
{
    return nn_cws_create (hint, epbase); 
}

static struct nn_optset *nn_ws_optset ()
{
    struct nn_ws_optset *optset;

    optset = nn_alloc (sizeof (struct nn_ws_optset), "optset (ws)");
    alloc_assert (optset);
    optset->base.vfptr = &nn_ws_optset_vfptr;

    /*  Default values for WebSocket options. */
    optset->placeholder = 1000;

    return &optset->base;   
}

static void nn_ws_optset_destroy (struct nn_optset *self)
{
    struct nn_ws_optset *optset;

    optset = nn_cont (self, struct nn_ws_optset, base);
    nn_free (optset);
}

static int nn_ws_optset_setopt (struct nn_optset *self, int option,
    const void *optval, size_t optvallen)
{
    struct nn_ws_optset *optset;

    optset = nn_cont (self, struct nn_ws_optset, base);

    switch (option) {
    case NN_WS_OPTION_PLACEHOLDER:
        if (optvallen != sizeof (int))
            return -EINVAL;
        optset->placeholder = *(int*) optval;
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

static int nn_ws_optset_getopt (struct nn_optset *self, int option,
    void *optval, size_t *optvallen)
{
    struct nn_ws_optset *optset;

    optset = nn_cont (self, struct nn_ws_optset, base);

    switch (option) {
    case NN_WS_OPTION_PLACEHOLDER:
        memcpy (optval, &optset->placeholder,
            *optvallen < sizeof (int) ? *optvallen : sizeof (int));
        *optvallen = sizeof (int);
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

int nn_ws_send (int s, const void *msg, size_t len, uint8_t msg_type, int flags)
{
    int rc;
    struct nn_iovec iov;
    struct nn_msghdr hdr;
    struct nn_cmsghdr *cmsg;
    size_t cmsgsz;

    iov.iov_base = (void*) msg;
    iov.iov_len = len;
    
    cmsgsz = NN_CMSG_SPACE (sizeof (msg_type));
    cmsg = nn_allocmsg (cmsgsz, 0);
    if (cmsg == NULL)
        return -1;

    cmsg->cmsg_level = NN_WS;
    cmsg->cmsg_type = NN_WS_HDR_OPCODE;
    cmsg->cmsg_len = NN_CMSG_LEN (sizeof (msg_type));
    memcpy (NN_CMSG_DATA (cmsg), &msg_type, sizeof (msg_type));

    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = &cmsg;
    hdr.msg_controllen = NN_MSG;

    rc = nn_sendmsg (s, &hdr, flags);

    return rc;
}

int nn_ws_recv (int s, void *msg, size_t len, uint8_t *msg_type, int flags)
{
    struct nn_iovec iov;
    struct nn_msghdr hdr;
    struct nn_cmsghdr *cmsg;
    void *cmsg_buf;
    int rc;

    iov.iov_base = msg;
    iov.iov_len = len;

    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = &cmsg_buf;
    hdr.msg_controllen = NN_MSG;

    rc = nn_recvmsg (s, &hdr, flags);
    if (rc < 0)
        return rc;

    /* Find WebSocket opcode ancillary property. */
    cmsg = NN_CMSG_FIRSTHDR (&hdr);
    while (cmsg) {
        if (cmsg->cmsg_level == NN_WS && cmsg->cmsg_type == NN_WS_HDR_OPCODE) {
            *msg_type = *(uint8_t *) NN_CMSG_DATA (cmsg);
            break;
        }
        cmsg = NN_CMSG_NXTHDR (&hdr, cmsg);
    }

    /*  WebSocket transport should always report this header. */
    nn_assert (cmsg);

    /*  WebSocket transport should always reassemble fragmented messages. */
    nn_assert (*msg_type & NN_SWS_FRAME_BITMASK_FIN);

    /*  Return only the message type (opcode). */
    if (*msg_type == (NN_WS_MSG_TYPE_GONE | NN_SWS_FRAME_BITMASK_FIN))
        *msg_type = NN_WS_MSG_TYPE_GONE;
    else
        *msg_type &= NN_SWS_FRAME_BITMASK_OPCODE;

    nn_freemsg (cmsg_buf);

    return rc;
}
