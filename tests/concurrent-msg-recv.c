/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* This is meant to be a template file to use when developing new LPs. 
 * Roughly follows the format of the existing LPs in the CODES repos */

#include "codes/codes_mapping.h"
#include "codes/lp-type-lookup.h"
#include "codes/jenkins-hash.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include <string.h>
#include <assert.h>
#include <limits.h>

/**** BEGIN SIMULATION DATA STRUCTURES ****/

#define NUM_REQS 1

#define TEST_DEBUG 1

static int testsvr_magic; /* use this as sanity check on events */

int net_id;

typedef struct testsvr_state testsvr_state;
typedef struct testsvr_msg testsvr_msg;

/* event types */
enum testsvr_event
{
    KICKOFF,
    REQ,
    ACK,
    LOCAL, /* dummy to add some time between completion */
};

struct testsvr_state {
    int idx;
    int req_stat[NUM_REQS];

#if TEST_DEBUG
    FILE *fdebug;
    /* count the number of forward events processed so we know EXACTLY which
     * invocations of events cause events in other LPs */
    int event_ctr; 
#endif
};

struct testsvr_msg {
    int magic;
    enum testsvr_event event_type;
    int idx_src;
    tw_lpid lp_src;
    int req_num;
#if TEST_DEBUG
    /* event count that produced this message in the first place */
    int src_event_ctr;
#endif
};

/**** END SIMULATION DATA STRUCTURES ****/

/**** BEGIN LP, EVENT PROCESSING FUNCTION DECLS ****/

/* ROSS LP processing functions */  
static void testsvr_lp_init(
    testsvr_state * ns,
    tw_lp * lp);
static void testsvr_event_handler(
    testsvr_state * ns,
    tw_bf * b,
    testsvr_msg * m,
    tw_lp * lp);
static void testsvr_rev_handler(
    testsvr_state * ns,
    tw_bf * b,
    testsvr_msg * m,
    tw_lp * lp);
static void testsvr_finalize(
    testsvr_state * ns,
    tw_lp * lp);

/* event type handlers */
static void handle_testsvr_kickoff(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp);
static void handle_testsvr_req(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp);
static void handle_testsvr_ack(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp);
static void handle_testsvr_local(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp);
static void handle_testsvr_kickoff_rev(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp);
static void handle_testsvr_req_rev(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp);
static void handle_testsvr_ack_rev(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp);
static void handle_testsvr_local_rev(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp);

/* ROSS function pointer table for this LP */
tw_lptype testsvr_lp = {
    (init_f) testsvr_lp_init,
    (pre_run_f) NULL,
    (event_f) testsvr_event_handler,
    (revent_f) testsvr_rev_handler,
    (final_f)  testsvr_finalize, 
    (map_f) codes_mapping,
    sizeof(testsvr_state),
};

/* for debugging: print messages */
static void dump_msg(testsvr_msg *m, tw_lp *lp, FILE *f);
static void dump_state(tw_lp *lp, testsvr_state *ns, FILE *f);

/**** END LP, EVENT PROCESSING FUNCTION DECLS ****/

/**** BEGIN IMPLEMENTATIONS ****/

void testsvr_init(){
    uint32_t h1=0, h2=0;

    bj_hashlittle2("testsvr", strlen("testsvr"), &h1, &h2);
    testsvr_magic = h1+h2;

    lp_type_register("testsvr", &testsvr_lp);
}

void testsvr_lp_init(
        testsvr_state * ns,
        tw_lp * lp){
    /* for test, just use dummy way (assume 1 svr / 1 modelnet) */
    ns->idx = lp->gid / 2;
    
    /* expect exactly three servers */
    assert(ns->idx <= 2);

    memset(ns->req_stat, 0x0, NUM_REQS*sizeof(int));
    /* create kickoff event only if we're a request server */
    if (ns->idx == 0 || ns->idx == 2){
        tw_event *e = codes_event_new(lp->gid, codes_local_latency(lp), lp);
        testsvr_msg *m_local = tw_event_data(e);
        m_local->magic = testsvr_magic;
        m_local->event_type = KICKOFF;
        /* dummy values for kickoff */
        m_local->idx_src = INT_MAX;
        m_local->lp_src  = INT_MAX;
        m_local->req_num = INT_MAX;
        tw_event_send(e);
    }
#if TEST_DEBUG
    char name[32];
    sprintf(name, "testsvr.%d.%lu", ns->idx, lp->gid);
    ns->fdebug = fopen(name, "w");
    setvbuf(ns->fdebug, NULL, _IONBF, 0);
    assert(ns->fdebug != NULL);
    ns->event_ctr = 0;
#endif
}

/* test boilerplate helpers */
#if TEST_DEBUG 
#define DUMP_PRE(lp, ns, m, type) \
    do{ \
        fprintf(ns->fdebug, type); \
        dump_state(lp,ns,ns->fdebug); \
        dump_msg(m,lp,ns->fdebug); \
        fflush(ns->fdebug);\
    }while(0)

#define DUMP_POST(lp, ns, type) \
    do{ \
        fprintf(ns->fdebug, type); \
        dump_state(lp,ns,ns->fdebug); \
        fflush(ns->fdebug);\
    }while(0)

#else
#define DUMP_PRE(lp, ns, m, type, is_rev) do{}while(0)
#define DUMP_POST(lp, ns, is_rev) do{}while(0)
#endif

void testsvr_event_handler(
        testsvr_state * ns,
        tw_bf * b,
        testsvr_msg * m,
        tw_lp * lp){
    assert(m->magic == testsvr_magic);
    
    switch (m->event_type){
        case KICKOFF:
            DUMP_PRE(lp,ns,m,"== pre kickoff ==\n");
            handle_testsvr_kickoff(ns, m, lp);
            DUMP_POST(lp,ns,"== post kickoff ==\n");
            break;
        case REQ:
            DUMP_PRE(lp,ns,m,"== pre req ==\n");
            handle_testsvr_req(ns, m, lp);
            DUMP_POST(lp,ns,"== post req ==\n");
            break;
        case ACK:
            DUMP_PRE(lp,ns,m,"== pre ack ==\n");
            handle_testsvr_ack(ns, m, lp);
            DUMP_POST(lp,ns,"== post ack ==\n");
            break;
        case LOCAL:
            DUMP_PRE(lp,ns,m,"== pre local ==\n");
            handle_testsvr_local(ns, m, lp);
            DUMP_POST(lp,ns,"== post local ==\n");
            break;
        /* ... */
        default:
            assert(!"testsvr event type not known");
            break;
    }
}

void testsvr_rev_handler(
        testsvr_state * ns,
        tw_bf * b,
        testsvr_msg * m,
        tw_lp * lp){
    assert(m->magic == testsvr_magic);
    
    switch (m->event_type){
        case KICKOFF:
            DUMP_PRE(lp,ns,m,"== pre kickoff rev == ");
            handle_testsvr_kickoff_rev(ns, m, lp);
            DUMP_POST(lp,ns,"== post kickoff rev ==\n");
            break;
        case REQ:
            DUMP_PRE(lp,ns,m,"== pre req rev ==\n");
            handle_testsvr_req_rev(ns, m, lp);
            DUMP_POST(lp,ns,"== post req rev ==\n");
            break;
        case ACK:
            DUMP_PRE(lp,ns,m,"== pre ack rev ==\n");
            handle_testsvr_ack_rev(ns, m, lp);
            DUMP_POST(lp,ns,"== post ack rev ==\n");
            break;
        case LOCAL:
            DUMP_PRE(lp,ns,m,"== pre local rev ==\n");
            handle_testsvr_local_rev(ns, m, lp);
            DUMP_POST(lp,ns,"== post local rev ==\n");
            break;
        /* ... */
        default:
            assert(!"testsvr event type not known");
            break;
    }
}

void testsvr_finalize(
        testsvr_state * ns,
        tw_lp * lp){
    /* ensure that all requests are accounted for */
    int req_expected = (ns->idx == 1) ? 2 : 1;
    int req;
    for (req = 0; req < NUM_REQS; req++){
        assert(ns->req_stat[req] == req_expected);
    }
}

void handle_testsvr_kickoff(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp){

    assert(ns->idx == 0 || ns->idx == 2);
    int req;
    for (req = 0; req < NUM_REQS; req++){
        tw_lpid dest_lp = (1) * 2; /* send to server 1 */
        testsvr_msg m_net;
        m_net.magic = testsvr_magic;
        m_net.event_type = REQ;
        m_net.idx_src = ns->idx;
        m_net.lp_src = lp->gid;
#if TEST_DEBUG
        m_net.src_event_ctr = ns->event_ctr++;
#endif
        m_net.req_num = req;
        model_net_event(net_id, "req", dest_lp, 1, 0.0, sizeof(m_net), &m_net, 0, NULL, lp);
    }
#if TEST_DEBUG
    ns->event_ctr++;
#endif
}

void handle_testsvr_req(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp){

    /* only server 1 processes requests */
    assert(ns->idx == 1);
    /* add a random amount of time to it */
    tw_event *e = codes_event_new(lp->gid, codes_local_latency(lp), lp);
    testsvr_msg *m_local = tw_event_data(e);
    *m_local = *m;
    m_local->event_type = LOCAL;
#if TEST_DEBUG
    m_local->src_event_ctr = ns->event_ctr;
#endif

    tw_event_send(e);

#if TEST_DEBUG
    ns->event_ctr++;
#endif
}

void handle_testsvr_ack(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp){

    /* only servers 0 and 2 handle acks */
    assert(ns->idx == 0 || ns->idx == 2);

    ns->req_stat[m->req_num]++;
    assert(ns->req_stat[m->req_num] < 2);

#if TEST_DEBUG
    ns->event_ctr++;
#endif
}

void handle_testsvr_local(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp){

    assert(ns->idx == 1);
    
    testsvr_msg m_net;
    m_net.magic = testsvr_magic;
    m_net.event_type = ACK;
    m_net.idx_src = ns->idx;
    m_net.lp_src = lp->gid;
    m_net.req_num = m->req_num;
    tw_lpid dest_lp = m->idx_src * 2;
#if TEST_DEBUG
    m_net.src_event_ctr = ns->event_ctr;
#endif
    model_net_event(net_id, "ack", dest_lp,
            1, 0.0, sizeof(m_net), &m_net, 0, NULL, lp);
    ns->req_stat[m->req_num]++;
    /* we are handling exactly two reqs per slot */
    assert(ns->req_stat[m->req_num] <= 2);

#if TEST_DEBUG
    ns->event_ctr++;
#endif
}

void handle_testsvr_kickoff_rev(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp){
    int req;
    for (req = 0; req < NUM_REQS; req++){
        model_net_event_rc(net_id, lp, 1);
    }
}

void handle_testsvr_req_rev(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp){

    assert(ns->idx == 1);

    codes_local_latency_reverse(lp);
}

void handle_testsvr_ack_rev(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp){

    assert(ns->idx == 0 || ns->idx == 2);

    ns->req_stat[m->req_num]--;
    assert(ns->req_stat[m->req_num] >= 0);
}

void handle_testsvr_local_rev(
    testsvr_state * ns,
    testsvr_msg * m,
    tw_lp * lp){

    assert(ns->idx == 1);

    ns->req_stat[m->req_num]--;
    assert(ns->req_stat[m->req_num] >= 0);

    model_net_event_rc(net_id, lp, 1);
}

/* for debugging: print messages */
void dump_msg(testsvr_msg *m, tw_lp *lp, FILE *f){
    fprintf(f,"event: magic:%10d, src:%1d (LP:%lu), req:%1d, src_event_cnt:%2d, ts:%.5le\n",
            m->magic, m->idx_src, m->lp_src, m->req_num, m->src_event_ctr, tw_now(lp));
}

void dump_state(tw_lp *lp, testsvr_state *ns, FILE *f){
    char *buf = malloc(2048);
    int written = sprintf(buf, "idx:%d LP:%lu, event_cnt:%d, [%d", 
            ns->idx, lp->gid, ns->event_ctr, ns->req_stat[0]);
    int req;
    for (req = 1; req < NUM_REQS; req++){
        written += sprintf(buf+written, ",%d",ns->req_stat[req]);
    }
    sprintf(buf+written, "]\n");
    fprintf(f, "%s",buf);
    free(buf);
}

/**** END IMPLEMENTATIONS ****/

/**** BEGIN MAIN ****/

char conf_file_name[256] = {0};

const tw_optdef app_opt[] = {
    TWOPT_GROUP("ROSD mock test model"),
    TWOPT_CHAR("codes-config", conf_file_name, "Name of codes configuration file"),
    TWOPT_END()
};

tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}

int main(int argc, char *argv[])
{
    int num_nets, *net_ids;
    g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    if (!conf_file_name[0]) {
        fprintf(stderr, "Expected \"codes-config\" option, please see --help.\n");
        MPI_Finalize();
        return 1;
    }

    /* loading the config file into the codes-mapping utility, giving us the
     * parsed config object in return. 
     * "config" is a global var defined by codes-mapping */
    if (configuration_load(conf_file_name, MPI_COMM_WORLD, &config)){
        fprintf(stderr, "Error loading config file %s.\n", conf_file_name);
        MPI_Finalize();
        return 1;
    }

    /* currently restrict to simplenet, as other networks are trickier to
     * setup. TODO: handle other networks properly */
    if(net_id != SIMPLENET) {
        printf("\n The test works with simple-net configuration only! ");
        MPI_Finalize();
        return 1;
    }

    testsvr_init();
    model_net_register();

    codes_mapping_setup();

    /* Setup the model-net parameters specified in the global config object,
     * returned are the identifier for the network type */
    net_ids = model_net_configure(&num_nets);
    assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);

    tw_run();
    tw_end();

    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
