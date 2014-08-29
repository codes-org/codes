/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY:
 *
 * This is a test harness for the lp-io API.  It sets up a number of LPs and 
 * has each of them write some portion of various data  sets.
 *
 */

#include <string.h>
#include <assert.h>
#include <ross.h>

#include "codes/lp-io.h"
#include "codes/codes.h"

#define NUM_SERVERS 16  /* number of servers */

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

enum svr_event_type
{
    KICKOFF,    /* initial event */
};

struct svr_state
{
};

struct svr_msg
{
    enum svr_event_type event_type;
    tw_lpid src;          /* source of this request or ack */
};

const tw_optdef app_opt[] = {
    TWOPT_GROUP("lp-io Test Model"),
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

static void handle_kickoff_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_kickoff_event(
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
    int lps_per_proc;
    int i;
    int ret;
    lp_io_handle handle;

    g_tw_ts_end = 60*60*24*365;

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);
 
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  
    if((NUM_SERVERS) % nprocs)
    {
        fprintf(stderr, "Error: number of server LPs (%d total) is not evenly divisible by the number of MPI processes (%d)\n", NUM_SERVERS, nprocs);
        exit(-1);
    }

    lps_per_proc = (NUM_SERVERS) / nprocs;

    tw_define_lps(lps_per_proc, sizeof(struct svr_msg), 0);

    for(i=0; i<lps_per_proc; i++)
    {
        tw_lp_settype(i, &svr_lp);
    }

    g_tw_lookahead = 100;

    ret = lp_io_prepare("lp-io-test-results", LP_IO_UNIQ_SUFFIX, &handle, MPI_COMM_WORLD);
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

    /* each server sends a dummy event to itself */

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
        case KICKOFF:
            handle_kickoff_event(ns, b, m, lp);
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
        case KICKOFF:
            handle_kickoff_rev_event(ns, b, m, lp);
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

    return;
}

static tw_peid svr_node_mapping(
    tw_lpid gid)
{
    return (tw_peid) gid / g_tw_nlp;
}

static void handle_kickoff_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    assert(0);

    return;
}

/* handle initial event */
static void handle_kickoff_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    //printf("handle_kickoff_event(), lp %llu.\n", (unsigned long long)lp->gid);

    /* do nothing */
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
