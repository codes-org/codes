/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY: This is a compute node LP to be used in a codes workload
 * test/demo
 */

#include <string.h>
#include <assert.h>
#include <ross.h>

#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes-workload.h"
#include "codes-workload-test-cn-lp.h"
#include "codes-workload-test-svr-lp.h"

typedef struct client_msg client_msg;
typedef struct client_state client_state;

enum client_event_type
{
    CLIENT_KICKOFF = 64,    /* initial event */
    CLIENT_OP_COMPLETE, /* finished previous I/O operation */
    CLIENT_OP_BARRIER, /* event received at root to indicate barrier entry */
};

struct client_state
{
    int my_rank;
    int wkld_id;
    int target_barrier_count;
    int current_barrier_count;
};

struct client_msg
{
    enum client_event_type event_type;
    int barrier_count;
};

static void handle_client_op_loop_rev_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp);
static void handle_client_op_loop_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp);
static void handle_client_op_barrier_rev_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp);
static void handle_client_op_barrier_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp);
static void cn_enter_barrier(tw_lp *lp, tw_lpid gid, int count);

static void client_init(
    client_state * ns,
    tw_lp * lp);
static void client_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp);
static void client_rev_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp);
static void client_finalize(
    client_state * ns,
    tw_lp * lp);
static tw_peid node_mapping(
    tw_lpid gid);

tw_lptype client_lp = {
     (init_f) client_init,
     (event_f) client_event,
     (revent_f) client_rev_event,
     (final_f) client_finalize, 
     (map_f) node_mapping,
     sizeof(client_state),
};

static int g_num_clients = -1;
static int g_num_servers = -1;

static void client_init(
    client_state * ns,
    tw_lp * lp)
{
    tw_event *e;
    client_msg *m;
    tw_stime kickoff_time;
    
    memset(ns, 0, sizeof(*ns));
    ns->my_rank = lp->gid;

    /* each client sends a dummy event to itself */

    /* skew each kickoff event slightly to help avoid event ties later on */
    kickoff_time = g_tw_lookahead + tw_rand_unif(lp->rng); 

    e = codes_event_new(lp->gid, kickoff_time, lp);
    m = tw_event_data(e);
    m->event_type = CLIENT_KICKOFF;
    tw_event_send(e);

    return;
}

static void client_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp)
{

    switch (m->event_type)
    {
        case CLIENT_KICKOFF:
        case CLIENT_OP_COMPLETE:
            handle_client_op_loop_event(ns, b, m, lp);
            break;
        case CLIENT_OP_BARRIER:
            handle_client_op_barrier_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }
}

static void client_rev_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp)
{
    switch (m->event_type)
    {
        case CLIENT_KICKOFF:
        case CLIENT_OP_COMPLETE:
            handle_client_op_loop_rev_event(ns, b, m, lp);
            break;
        case CLIENT_OP_BARRIER:
            handle_client_op_barrier_rev_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }

    return;
}

static void client_finalize(
    client_state * ns,
    tw_lp * lp)
{
    char buffer[256];
    int ret;

    /* write out some statistics (the current time of each cn as it
     * shuts down)
     */
    sprintf(buffer, "cn_lp:%ld\tfinalize_time:%f\n", (long)lp->gid, tw_now(lp));

    ret = lp_io_write(lp->gid, "compute_nodes", strlen(buffer)+1, buffer);
    assert(ret == 0);

    return;
}

static tw_peid node_mapping(
    tw_lpid gid)
{
    return (tw_peid) gid / g_tw_nlp;
}

static void handle_client_op_barrier_rev_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp)
{
    /* TODO: fill this in */
    assert(0);

    return;
}

static void handle_client_op_loop_rev_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp)
{
    /* TODO: fill this in */
    assert(0);

    return;
}

/* handle barrier */
static void handle_client_op_barrier_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp)
{
    tw_event *e;
    client_msg *m_out;
    int i;

    assert(ns->target_barrier_count == 0 || ns->target_barrier_count == m->barrier_count);
    if(ns->target_barrier_count == 0)
    {
        ns->target_barrier_count = m->barrier_count;
        ns->current_barrier_count = 0;
    }

    ns->current_barrier_count++;

    if(ns->current_barrier_count == ns->target_barrier_count)
    {
        /* release all clients, including self */
        for(i=0; i<ns->current_barrier_count; i++)
        {
            e = codes_event_new(lp->gid+i, codes_local_latency(lp), lp);
            m_out = tw_event_data(e);
            m_out->event_type = CLIENT_OP_COMPLETE;
            tw_event_send(e);
        }
        ns->current_barrier_count=0;
        ns->target_barrier_count=0;
    }

    return;
}

/* event indicates that we can issue the next operation */
static void handle_client_op_loop_event(
    client_state * ns,
    tw_bf * b,
    client_msg * m,
    tw_lp * lp)
{
    struct codes_workload_op op;
    tw_lpid dest_svr_id;

    printf("handle_client_op_loop_event(), lp %llu.\n", (unsigned long long)lp->gid);

    if(m->event_type == CLIENT_KICKOFF)
    {
        /* first operation; initialize the desired workload generator */
        ns->wkld_id = codes_workload_load("test", NULL, ns->my_rank);
        assert(ns->wkld_id > -1);
    }

    codes_workload_get_next(ns->wkld_id, ns->my_rank, &op);

    /* NOTE: in this test model the LP is doing its own math to find the LP
     * ID of servers just to do something simple.  It knows that compute
     * nodes are the first N LPs and servers are the next M LPs.
     */

    switch(op.op_type)
    {
        case CODES_WK_END:
            printf("Client rank %d completed workload.\n", ns->my_rank);
            /* stop issuing events; we are done */
            return;
            break;
        case CODES_WK_BARRIER:
            printf("Client rank %d hit barrier.\n", ns->my_rank);
            cn_enter_barrier(lp, op.u.barrier.root, op.u.barrier.count);
            return; 
            break;
        /* "normal" io operations: we just calculate the destination and
         * then continue after the switch block to send the specified
         * operation to a server.
         */
        case CODES_WK_OPEN:
            printf("Client rank %d will issue an open request.\n", ns->my_rank);
            dest_svr_id = g_num_clients + op.u.open.file_id % g_num_servers;
            break;
        default:
            assert(0);
            break;
    }

    svr_op_start(lp, dest_svr_id, &op);

    return;
}

static void cn_enter_barrier(tw_lp *lp, tw_lpid gid, int count)
{
    tw_event *e;
    client_msg *m_out;

    e = codes_event_new(gid, codes_local_latency(lp), lp);
    m_out = tw_event_data(e);
    m_out->event_type = CLIENT_OP_BARRIER;
    if(count == -1)
        m_out->barrier_count = g_num_clients;
    else
        m_out->barrier_count = count;
    tw_event_send(e);

    return;
}


void cn_op_complete(tw_lp *lp, tw_stime svc_time, tw_lpid gid)
{
    tw_event *e;
    client_msg *m;

    e = codes_event_new(gid, codes_local_latency(lp) + svc_time, lp);
    m = tw_event_data(e);
    m->event_type = CLIENT_OP_COMPLETE;
    tw_event_send(e);

    return;
}

void cn_op_complete_rc(tw_lp *lp)
{
    codes_local_latency_reverse(lp);

    return;
}

void cn_set_params(int num_clients, int num_servers)
{
    g_num_clients = num_clients;
    g_num_servers = num_servers;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
