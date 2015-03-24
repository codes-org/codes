/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY:
 *
 */

#include <string.h>
#include <assert.h>
#include <ross.h>

#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"

#include "codes/local-storage-model.h"

#define NUM_REQS 2000  /* number of requests sent by each server */
#define PAYLOAD_SZ (1024*1024) /* size of simulated data payload, bytes  */

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

/* types of events that will constitute triton requests */
enum svr_event_type
{
    KICKOFF,    /* initial event */
    REQ,        /* request event */
    ACK,        /* ack event */
    LOCAL,      /* local completion of a send */
};

struct svr_state
{
    int msg_sent_count;   /* requests sent */
    int msg_recvd_count;  /* requests recvd */
    tw_stime start_ts;    /* time that we started sending requests */
};

struct svr_msg
{
    enum svr_event_type event_type;
    tw_lpid src;          /* source of this request or ack */

    int incremented_flag; /* helper for reverse computation */
};

char conf_file_name[256] = {0};

const tw_optdef app_opt[] = {
    TWOPT_GROUP("Simple Network Test Model"),
    TWOPT_CHAR("conf", conf_file_name, "Name of configuration file"),
    TWOPT_END()
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
static tw_peid svr_node_mapping(
    tw_lpid gid);

tw_lptype svr_lp = {
    (init_f) svr_init,
    (pre_run_f) NULL,
    (event_f) svr_event,
    (revent_f) svr_rev_event,
    (final_f) svr_finalize, 
    (map_f) svr_node_mapping,
    sizeof(svr_state),
};

static tw_stime ns_to_s(tw_stime ns);
static tw_stime s_to_ns(tw_stime ns);
static void handle_kickoff_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_ack_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_req_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_kickoff_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_ack_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_req_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_local_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);

int main(
    int argc,
    char **argv)
{
    int nprocs;
    int rank;
    lp_io_handle handle;
    int ret;

    g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);
 
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
   
    /* read in configuration file */
    ret = configuration_load(conf_file_name, MPI_COMM_WORLD, &config);
    if (ret)
    {
        fprintf(stderr, "Error opening config file: %s\n", conf_file_name);
        return(-1);
    }

    lp_type_register("server", &svr_lp);
    lsm_register();

    codes_mapping_setup();

    lsm_configure();

    ret = lp_io_prepare("lsm-test", LP_IO_UNIQ_SUFFIX, &handle, MPI_COMM_WORLD);
    if(ret < 0)
    {
       return(-1); 
    }

    tw_run();

    ret = lp_io_flush(handle, MPI_COMM_WORLD);
    assert(ret == 0);

    tw_end();

    return 0;
}

static void svr_init(
    svr_state * ns,
    tw_lp * lp)
{
    tw_event *e;
    svr_msg *m;
    tw_stime kickoff_time;
    
    memset(ns, 0, sizeof(*ns));

    /* each server sends a dummy event to itself that will kick off the real
     * simulation
     */

    /* skew each kickoff event slightly to help avoid event ties later on */
    kickoff_time = g_tw_lookahead + tw_rand_unif(lp->rng); 

    e = codes_event_new(lp->gid, kickoff_time, lp);
    m = tw_event_data(e);
    m->event_type = KICKOFF;
    tw_event_send(e);

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
        case REQ:
            handle_req_event(ns, b, m, lp);
            break;
        case ACK:
            handle_ack_event(ns, b, m, lp);
            break;
        case KICKOFF:
            handle_kickoff_event(ns, b, m, lp);
            break;
        case LOCAL:
            handle_local_event(ns, b, m, lp);
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
        case REQ:
            handle_req_rev_event(ns, b, m, lp);
            break;
        case ACK:
            handle_ack_rev_event(ns, b, m, lp);
            break;
        case KICKOFF:
            handle_kickoff_rev_event(ns, b, m, lp);
            break;
        case LOCAL:
            /* NOTE: nothing to reverse here. */
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
    printf("server %llu : size:%d requests:%d time:%lf rate:%lf\n",
           (unsigned long long)lp->gid,
           PAYLOAD_SZ,
           NUM_REQS,
           ns_to_s((tw_now(lp)-ns->start_ts)),
           (double)(PAYLOAD_SZ*NUM_REQS)/(1024.0*1024.0)/ns_to_s((tw_now(lp)-ns->start_ts)));
    return;
}

static tw_peid svr_node_mapping(
    tw_lpid gid)
{
    return (tw_peid) gid / g_tw_nlp;
}

/* convert ns to seconds */
static tw_stime ns_to_s(tw_stime ns)
{
    return(ns / (1000.0 * 1000.0 * 1000.0));
}

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}

/* handle initial event */
static void handle_kickoff_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    svr_msg * m_new;
    tw_event *e_new;
    double rate;
    double seek;

    if (LSM_DEBUG)
        printf("handle_kickoff_event(), lp %llu.\n",
            (unsigned long long)lp->gid);

    /* record when transfers started on this server */
    ns->start_ts = tw_now(lp);

    rate = 50.0;
    seek = 2000.0;
    printf("server %llu : disk_rate:%lf disk_seek:%lf\n",
           (unsigned long long)lp->gid,
           rate,
           seek);

    e_new = lsm_event_new("test", lp->gid, 0, 0, PAYLOAD_SZ, LSM_WRITE_REQUEST, sizeof(svr_msg), lp, 1.0);
    m_new = lsm_event_data(e_new);
    m_new->event_type = ACK;
    m_new->src = lp->gid;
    ns->msg_sent_count++;
    
    tw_event_send(e_new);
}

/* reverse handler for req event */
static void handle_req_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    ns->msg_recvd_count--;

    return;
}


/* reverse handler for kickoff */
static void handle_kickoff_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{

    lsm_event_new_reverse(lp);

    ns->msg_sent_count--;

    return;
}


/* reverse handler for ack */
static void handle_ack_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    if(m->incremented_flag)
    {
        lsm_event_new_reverse(lp);
        ns->msg_sent_count--;
    }

    return;
}

/* handle recving ack */
static void handle_ack_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    svr_msg * m_new;
    tw_event *e_new;

    if (LSM_DEBUG)
        printf("handle_ack_event(), lp %llu.\n",
            (unsigned long long)lp->gid);

    /* safety check that this request got to the right server */
    assert(m->src == lp->gid);

    if(ns->msg_sent_count < NUM_REQS)
    {
        /* send another request */
        e_new = lsm_write_event_new("test", lp->gid, 0, 0, PAYLOAD_SZ, sizeof(svr_msg), lp);
        m_new = lsm_event_data(e_new);
        m_new->event_type = ACK;
        m_new->src = lp->gid;
        ns->msg_sent_count++;
        m->incremented_flag = 1;
        tw_event_send(e_new);
    }
    else
    {
        m->incremented_flag = 0;
    }

    return;
}

/* handle receiving request */
static void handle_req_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    svr_msg * m_new;
    tw_event *e_new;

    if (LSM_DEBUG)
        printf("handle_req_event(), lp %llu.\n",
            (unsigned long long)lp->gid);

    /* safety check that this request got to the right server */
    assert(lp->gid == m->src);

    ns->msg_recvd_count++;

    /* send ack back */
    e_new = codes_event_new(lp->gid, 0.00001, lp);
    m_new = tw_event_data(e_new);
    m_new->event_type = ACK;
    m_new->src = lp->gid;

    tw_event_send(e_new);

    return;
}

/* handle notification of local send completion */
static void handle_local_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    if (LSM_DEBUG)
        printf("handle_local_event(), lp %llu.\n",
            (unsigned long long)lp->gid);

    /* safety check that this request got to the right server */
    assert(lp->gid == m->src);

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
