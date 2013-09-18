/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* Summary: example storage server LP type for workload demo/test */

#include <string.h>
#include <assert.h>
#include <ross.h>

#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes-workload.h"
#include "codes-workload-test-svr-lp.h"
#include "codes-workload-test-cn-lp.h"

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

enum svr_event_type
{
    SVR_OP,
};

struct svr_state
{
};

struct svr_msg
{
    enum svr_event_type event_type;
    struct codes_workload_op op;
    tw_lpid src;          /* source of this request or ack */
};

static void svr_init(
    svr_state * ns,
    tw_lp * lp);
static void svr_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void svr_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void svr_finalize(
    svr_state * ns,
    tw_lp * lp);
static tw_peid node_mapping(
    tw_lpid gid);

tw_lptype svr_lp = {
     (init_f) svr_init,
     (event_f) svr_event,
     (revent_f) svr_rev_event,
     (final_f) svr_finalize, 
     (map_f) node_mapping,
     sizeof(svr_state),
};

static void handle_svr_op_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_svr_op_event_rc(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);


static void svr_init(
    svr_state * ns,
    tw_lp * lp)
{
    memset(ns, 0, sizeof(*ns));

    return;
}

static void svr_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{

    switch (m->event_type)
    {
        case SVR_OP:
            handle_svr_op_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }
}

static void svr_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    switch (m->event_type)
    {
        case SVR_OP:
            handle_svr_op_event_rc(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }

    return;
}

static void svr_finalize(
    svr_state * ns,
    tw_lp * lp)
{
#if 0
    char buffer[256];
    int ret;

    sprintf(buffer, "LP %ld finalize data\n", (long)lp->gid);

    /* test having everyone write to same identifier */
    ret = lp_io_write(lp->gid, "node_state_pointers", strlen(buffer)+1, buffer);
    assert(ret == 0);

    /* test having only one lp write to a particular identifier */
    if(lp->gid == 3)
    {
        ret = lp_io_write(lp->gid, "subset_example", strlen(buffer)+1, buffer);
        assert(ret == 0);
    }

    /* test having one lp write two buffers to the same id */
    if(lp->gid == 5)
    {
        sprintf(buffer, "LP %ld finalize data (intentional duplicate)\n", (long)lp->gid);
        ret = lp_io_write(lp->gid, "node_state_pointers", strlen(buffer)+1, buffer);
        assert(ret == 0);
    }
#endif

    return;
}

static tw_peid node_mapping(
    tw_lpid gid)
{
    return (tw_peid) gid / g_tw_nlp;
}

void svr_op_start(tw_lp *lp, tw_lpid gid, const struct codes_workload_op *op)
{
    tw_event *e;
    svr_msg *m;

    e = codes_event_new(gid, codes_local_latency(lp), lp);
    m = tw_event_data(e);
    m->event_type = SVR_OP;
    m->op = *op;
    m->src = lp->gid;
    tw_event_send(e);
}

void svr_op_start_rc(tw_lp *lp)
{
    codes_local_latency_reverse(lp);
}

static void handle_svr_op_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    /* TODO: fill in some stub service time */

    /* send event back to cn to let it know the operation is done */
    cn_op_complete(lp, m->src);

    return;
}

static void handle_svr_op_event_rc(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    assert(0);
    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
