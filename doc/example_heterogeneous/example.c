/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "codes/codes_mapping.h"
#include "codes/lp-type-lookup.h"
#include "codes/jenkins-hash.h"
#include "codes/codes.h"
#include "codes/lp-msg.h"
#include <codes/model-net.h>
#include <assert.h>

/**** BEGIN SIMULATION DATA STRUCTURES ****/

/* 'magic' numbers used as sanity check on events */
static int node_magic; 
static int forwarder_magic;

/* counts of the various types of nodes in the example system */
static int num_foo_nodes, num_bar_nodes;
static int num_foo_forwarders, num_bar_forwarders;

/* pings to perform (provided by config file) */
static int      num_pings;
static uint64_t payload_sz;

/* network type for the various clusters */
static int net_id_foo, net_id_bar, net_id_forwarding;

/* event types */
enum node_event
{
    NODE_KICKOFF = 123,
    NODE_RECV_PING,
    NODE_RECV_PONG,
};

typedef struct node_state_s {
    int is_in_foo;    // whether we're in foo's or bar's cluster
    int id_clust;     // my index within the cluster
    int num_processed; // number of requests processed
} node_state;

typedef struct node_msg_s {
    msg_header h;
    int id_clust_src;
} node_msg;

enum forwarder_event
{
    FORWARDER_FWD = 234,
    FORWARDER_RECV,
};

typedef struct forwarder_state_s {
    int id; // index w.r.t. forwarders in my group 
    int is_in_foo;    
    int fwd_node_count;
    int fwd_forwarder_count;
} forwarder_state;

typedef struct forwarder_msg_s {
    msg_header h;
    int src_node_clust_id;
    int dest_node_clust_id;
    enum node_event node_event_type;
} forwarder_msg;

/**** END SIMULATION DATA STRUCTURES ****/

/**** BEGIN IMPLEMENTATIONS ****/

void node_lp_init(
        node_state * ns,
        tw_lp * lp){
    ns->num_processed = 0;
    // nodes are addressed in their logical id space (0...num_foo_nodes-1 and
    // 0...num_bar_nodes-1, respectively). LPs are computed upon use with
    // model-net, other events
    ns->id_clust = codes_mapping_get_lp_relative_id(lp->gid, 1, 0);
    int id_all = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    // track which cluster we're in
    ns->is_in_foo = (id_all < num_foo_nodes);

    // send a self kickoff event
    tw_event *e = codes_event_new(lp->gid, codes_local_latency(lp), lp);
    node_msg *m = tw_event_data(e);
    msg_set_header(node_magic, NODE_KICKOFF, lp->gid, &m->h);
    tw_event_send(e);
}

void node_finalize(
        node_state * ns,
        tw_lp * lp){
    // do some error checking - here, we ensure we got the expected number of
    // messages
    int mult;
    if (ns->is_in_foo){
        mult = 1; 
    }
    else{
        mult = (num_foo_nodes / num_bar_nodes) +
            ((num_foo_nodes % num_bar_nodes) > ns->id_clust);
    }
    if (ns->num_processed != num_pings*mult){
        fprintf(stderr,
                "%s node %d, lp %lu: processed %d (expected %d)\n",
                ns->is_in_foo ? "foo" : "bar", ns->id_clust, lp->gid,
                ns->num_processed, num_pings*mult);
    }
}

/* event type handlers */
void handle_node_next(
        node_state * ns,
        node_msg * m,
        tw_lp * lp){
    // we must be in cluster foo for this function
    assert(ns->is_in_foo);

    // generate a message to send to the forwarder
    forwarder_msg m_fwd;
    msg_set_header(forwarder_magic, FORWARDER_FWD, lp->gid, &m_fwd.h);

    m_fwd.src_node_clust_id  = ns->id_clust;
    // compute the destination in cluster bar to ping based on a simple modulo
    // of the logical indexes
    m_fwd.dest_node_clust_id = ns->id_clust % num_bar_nodes;
    m_fwd.node_event_type = NODE_RECV_PING;

    // compute the dest forwarder index, again using a simple modulo
    int dest_fwd_id = ns->id_clust % num_foo_forwarders;

    // as the relative forwarder IDs are with respect to groups, the group
    // name must be used
    tw_lpid dest_fwd_lpid = codes_mapping_get_lpid_from_relative(dest_fwd_id,
            "FOO_FORWARDERS", "forwarder", NULL, 0);

    // as cluster nodes have only one network type (+ annotation), no need to
    // use annotation-specific messaging
    model_net_event_annotated(net_id_foo, "foo", "ping", dest_fwd_lpid,
            payload_sz, 0.0, sizeof(m_fwd), &m_fwd, 0, NULL, lp);
}

void handle_node_recv_ping(
        node_state * ns,
        node_msg * m,
        tw_lp * lp){
    // we must be in cluster bar to receive pings
    assert(!ns->is_in_foo);

    // check that we received the msg from the expected source
    assert(m->id_clust_src % num_bar_nodes == ns->id_clust);

    // setup the response message through the forwarder
    forwarder_msg m_fwd;
    msg_set_header(forwarder_magic, FORWARDER_FWD, lp->gid, &m_fwd.h);
    m_fwd.src_node_clust_id = ns->id_clust;
    m_fwd.dest_node_clust_id = m->id_clust_src;
    m_fwd.node_event_type = NODE_RECV_PONG;

    // compute the dest forwarder index, again using a simple modulus
    int dest_fwd_id = ns->id_clust % num_bar_forwarders;

    // as the relative forwarder IDs are with respect to groups, the group
    // name must be used
    tw_lpid dest_fwd_lpid = codes_mapping_get_lpid_from_relative(dest_fwd_id,
            "BAR_FORWARDERS", "forwarder", NULL, 0);

    model_net_event_annotated(net_id_bar, "bar", "pong", dest_fwd_lpid,
            payload_sz, 0.0, sizeof(m_fwd), &m_fwd, 0, NULL, lp);

    ns->num_processed++;
}

void handle_node_recv_pong(
        node_state * ns,
        node_msg * m,
        tw_lp * lp){
    // we must be in cluster foo
    assert(ns->id_clust < num_foo_nodes);

    // simply process the next message
    ns->num_processed++;
    if (ns->num_processed < num_pings){
        handle_node_next(ns, m, lp);
    }
}

void node_event_handler(
        node_state * ns,
        tw_bf * b,
        node_msg * m,
        tw_lp * lp){
    assert(m->h.magic == node_magic);
    
    switch (m->h.event_type){
        case NODE_KICKOFF:
            // nodes from foo ping to nodes in bar
            if (ns->is_in_foo){
                handle_node_next(ns, m, lp);
            }
            break;
        case NODE_RECV_PING:
            handle_node_recv_ping(ns, m, lp);
            break;
        case NODE_RECV_PONG:
            handle_node_recv_pong(ns, m, lp);
            break;
        /* ... */
        default:
            tw_error(TW_LOC, "node event type not known");
    }
}

/* ROSS function pointer table for this LP */
static tw_lptype node_lp = {
    (init_f) node_lp_init,
    (pre_run_f) NULL,
    (event_f) node_event_handler,
    (revent_f) NULL,
    (final_f)  node_finalize, 
    (map_f) codes_mapping,
    sizeof(node_state),
};

void node_register(){
    uint32_t h1=0, h2=0;

    bj_hashlittle2("node", strlen("node"), &h1, &h2);
    node_magic = h1+h2;

    lp_type_register("node", &node_lp);
}


/*** Forwarder LP ***/

void forwarder_lp_init(
        forwarder_state * ns,
        tw_lp * lp){
    // like nodes, forwarders in this example are addressed logically 
    ns->id = codes_mapping_get_lp_relative_id(lp->gid, 1, 0);
    int id_all = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    ns->is_in_foo = (id_all < num_foo_forwarders);
}

void forwarder_finalize(
        forwarder_state * ns,
        tw_lp * lp){
    // nothing to see here
}

void handle_forwarder_fwd(
        forwarder_state * ns,
        forwarder_msg * m,
        tw_lp * lp){
    // compute the forwarder lpid to forward to 
    int mod;
    const char * dest_group;
    char * category;
    if (ns->is_in_foo){
        mod = num_bar_forwarders;
        dest_group = "BAR_FORWARDERS";
        category = "ping";
    }
    else{
        mod = num_foo_forwarders;
        dest_group = "FOO_FORWARDERS";
        category = "pong";
    }

    // compute the ROSS id corresponding to the dest forwarder
    tw_lpid dest_lpid = codes_mapping_get_lpid_from_relative(
            ns->id % mod, dest_group, "forwarder", NULL, 0);

    forwarder_msg m_fwd = *m;
    msg_set_header(forwarder_magic, FORWARDER_RECV, lp->gid, &m_fwd.h);

    // here, we need to use the unannotated forwarding network, so we
    // use the annotation version of model_net_event
    model_net_event_annotated(net_id_forwarding, NULL, category, dest_lpid,
            payload_sz, 0.0, sizeof(m_fwd), &m_fwd, 0, NULL, lp);

    ns->fwd_node_count++;
}

void handle_forwarder_recv(
        forwarder_state * ns,
        forwarder_msg * m,
        tw_lp * lp) {
    // compute the node to relay the message to 
    const char * dest_group;
    const char * annotation;
    char * category;
    int net_id;
    if (ns->is_in_foo){
        dest_group = "FOO_CLUSTER";    
        annotation = "foo";
        category = "pong";
        net_id = net_id_foo;
    }
    else{
        dest_group = "BAR_CLUSTER";
        annotation = "bar";
        category = "ping";
        net_id = net_id_bar;
    }

    tw_lpid dest_lpid = codes_mapping_get_lpid_from_relative(
                m->dest_node_clust_id, dest_group, "node",
                NULL, 0);

    node_msg m_node;
    msg_set_header(node_magic, m->node_event_type, lp->gid, &m_node.h);
    m_node.id_clust_src = m->src_node_clust_id;

    // here, we need to use the foo or bar cluster's internal network, so we use
    // the annotated version of model_net_event
    model_net_event_annotated(net_id, annotation, category, dest_lpid,
            payload_sz, 0.0, sizeof(m_node), &m_node, 0, NULL, lp);

    ns->fwd_forwarder_count++;
}

void forwarder_event_handler(
        forwarder_state * ns,
        tw_bf * b,
        forwarder_msg * m,
        tw_lp * lp){
    assert(m->h.magic == forwarder_magic);

    switch(m->h.event_type){
        case FORWARDER_FWD:
            handle_forwarder_fwd(ns, m, lp);
            break;
        case FORWARDER_RECV:
            handle_forwarder_recv(ns, m, lp);
            break;
        default:
            tw_error(TW_LOC, "unknown forwarder event type");
    }
}

static tw_lptype forwarder_lp = {
    (init_f) forwarder_lp_init,
    (pre_run_f) NULL,
    (event_f) forwarder_event_handler,
    (revent_f) NULL,
    (final_f)  forwarder_finalize, 
    (map_f) codes_mapping,
    sizeof(forwarder_state),
};

void forwarder_register(){
    uint32_t h1=0, h2=0;

    bj_hashlittle2("forwarder", strlen("forwarder"), &h1, &h2);
    forwarder_magic = h1+h2;

    lp_type_register("forwarder", &forwarder_lp);
}

/**** END IMPLEMENTATIONS ****/

/* arguments to be handled by ROSS - strings passed in are expected to be
 * pre-allocated */
static char conf_file_name[256] = {0};
/* this struct contains default parameters used by ROSS, as well as
 * user-specific arguments to be handled by the ROSS config sys. Pass it in
 * prior to calling tw_init */
const tw_optdef app_opt [] =
{
	TWOPT_GROUP("Model net test case" ),
        TWOPT_CHAR("codes-config", conf_file_name, "name of codes configuration file"),
	TWOPT_END()
};

static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}

int main(int argc, char *argv[])
{
    g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

    /* ROSS initialization function calls */
    tw_opt_add(app_opt); /* add user-defined args */
    /* initialize ROSS and parse args. NOTE: tw_init calls MPI_Init */
    tw_init(&argc, &argv); 

    if (!conf_file_name[0]) {
        tw_error(TW_LOC, 
                "Expected \"codes-config\" option, please see --help.\n");
        return 1;
    }

    /* loading the config file into the codes-mapping utility, giving us the
     * parsed config object in return. 
     * "config" is a global var defined by codes-mapping */
    if (configuration_load(conf_file_name, MPI_COMM_WORLD, &config)){
        tw_error(TW_LOC, "Error loading config file %s.\n", conf_file_name);
        return 1;
    }

    /* register model-net LPs with ROSS */
    model_net_register();

    /* register the user LPs */
    node_register();
    forwarder_register();

    /* setup the LPs and codes_mapping structures */
    codes_mapping_setup();

    /* setup the globals */
    int rc = configuration_get_value_int(&config, "run_params", "num_reqs", NULL,
            &num_pings);
    if (rc != 0)
        tw_error(TW_LOC, "unable to read run_params:num_reqs");
    int payload_sz_d;
    rc = configuration_get_value_int(&config, "run_params", "payload_sz", NULL,
            &payload_sz_d);
    if (rc != 0)
        tw_error(TW_LOC, "unable to read run_params:payload_sz");
    payload_sz = (uint64_t) payload_sz_d;

    /* get the counts for the foo and bar clusters */
    num_foo_nodes = codes_mapping_get_lp_count("FOO_CLUSTER", 0, "node",
            NULL, 1);
    num_bar_nodes = codes_mapping_get_lp_count("BAR_CLUSTER", 0, "node",
            NULL, 1);
    num_foo_forwarders = codes_mapping_get_lp_count("FOO_FORWARDERS", 0,
            "forwarder", NULL, 1);
    num_bar_forwarders = codes_mapping_get_lp_count("BAR_FORWARDERS", 0,
            "forwarder", NULL, 1);


    /* Setup the model-net parameters specified in the global config object,
     * returned are the identifier(s) for the network type.
     * 1 ID  -> all the same modelnet model
     * 2 IDs -> clusters are the first id, forwarding network the second
     * 3 IDs -> cluster foo is the first, bar is the second,
     *          forwarding network the third */
    int num_nets;
    int *net_ids = model_net_configure(&num_nets);
    assert(num_nets <= 3);
    if (num_nets == 1) {
        net_id_foo = net_ids[0];
        net_id_bar = net_ids[0];
        net_id_forwarding = net_ids[0];
    }
    else if (num_nets == 2) {
        net_id_foo = net_ids[0];
        net_id_bar = net_ids[0];
        net_id_forwarding = net_ids[1];
    }
    else {
        net_id_foo = net_ids[0];
        net_id_bar = net_ids[1];
        net_id_forwarding = net_ids[2];
    }
    free(net_ids);

    /* begin simulation */ 
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
