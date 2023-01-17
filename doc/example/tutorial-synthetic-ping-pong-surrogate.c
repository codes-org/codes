/*
 * Copyright (C) 2019 Neil McGlohon
 * Mantained/edited by Elkin Cruz (2022-2023)
 * See LICENSE notice in top-level directory
 */

#include "codes/model-net.h"
#include "codes/codes_mapping.h"
#include "codes/surrogate.h"
#include "codes/net/dragonfly-dally.h"


static int net_id = 0;
static int PAYLOAD_SZ = 4096;
static unsigned long long num_nodes = 0;

static char lp_io_dir[256] = {'\0'};
static lp_io_handle io_handle;
static unsigned int lp_io_use_suffix = 0;
static int do_lp_io = 0;

static int num_msgs = 20;

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

/* global variables for codes mapping */
static char group_name[MAX_NAME_LENGTH];
static char lp_type_name[MAX_NAME_LENGTH];
static int group_index, lp_type_index, rep_id, offset;

/* type of events */
enum svr_event
{
    KICKOFF = 1,
    PING,
    PONG
};

struct svr_msg
{
    enum svr_event svr_event_type; //KICKOFF, PING, or PONG
    int sender_id; //ID of the sender workload LP to know who to send a PONG message back to
    int payload_value; //Some value that we will encode as an example
    model_net_event_return event_rc; //helper to encode data relating to CODES rng usage
};

struct svr_state
{
    tw_lpid svr_id;            /* the ID of this server */
    int ping_msg_sent_count;   /* PING messages sent */
    int ping_msg_recvd_count;  /* PING messages received */
    int pong_msg_sent_count;   /* PONG messages sent */
    int pong_msg_recvd_count;  /* PONG messages received */
    tw_stime start_ts;    /* time that this LP started sending requests */
    tw_stime end_ts;      /* time that this LP ended sending requests */
    int payload_sum;      /* the running sum of all payloads received */
};

/* declaration of functions */
static void svr_init(svr_state * s, tw_lp * lp);
static void svr_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp);
static void svr_rev_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp);
static void svr_commit(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp);
static void svr_finalize(svr_state * s, tw_lp * lp);
static tw_stime ns_to_s(tw_stime ns);
static tw_stime s_to_ns(tw_stime s);

/* ROSS lptype function callback mapping */
tw_lptype svr_lp = {
    (init_f) svr_init,
    (pre_run_f) NULL,
    (event_f) svr_event,
    (revent_f) svr_rev_event,
    (commit_f) svr_commit,
    (final_f)  svr_finalize,
    (map_f) codes_mapping,
    sizeof(svr_state),
};

const tw_optdef app_opt [] =
{
        TWOPT_GROUP("Model net synthetic traffic " ),
    	TWOPT_UINT("num_messages", num_msgs, "Number of PING messages to be generated per terminal "),
    	TWOPT_UINT("payload_sz",PAYLOAD_SZ, "size of the message being sent "),
        TWOPT_CHAR("lp-io-dir", lp_io_dir, "Where to place io output (unspecified -> no output"),
        TWOPT_UINT("lp-io-use-suffix", lp_io_use_suffix, "Whether to append uniq suffix to lp-io directory (default 0)"),
        TWOPT_END()
};

const tw_lptype* svr_get_lp_type()
{
    return(&svr_lp);
}

static void svr_add_lp_type()
{
  lp_type_register("nw-lp", svr_get_lp_type());
}

// === START OF surrogate functions
//
#define N_TERMINALS 72
struct latency_surrogate {
    double sum_latency[N_TERMINALS];
    unsigned int total_msgs[N_TERMINALS];
};

static void init_pred(struct latency_surrogate * data, tw_lp * lp, unsigned int src_terminal) {
    (void) lp;
    (void) src_terminal;
    assert(data->sum_latency[0] == 0);
    assert(data->total_msgs[0] == 0);
}

static void feed_pred(struct latency_surrogate * data, tw_lp * lp, unsigned int src_terminal, struct packet_start * start, struct packet_end * end) {
    (void) lp;
    (void) src_terminal;

    unsigned int const dest_terminal = start->dfdally_dest_terminal_id;
    double const latency = end->travel_end_time - start->travel_start_time;
    assert(dest_terminal < N_TERMINALS);

    data->sum_latency[dest_terminal] += latency;
    data->total_msgs[dest_terminal]++;
}

static double predict_latency(struct latency_surrogate * data, tw_lp * lp, unsigned int src_terminal, struct packet_start * packet_dest) {
    (void) lp;

    unsigned int const dest_terminal = packet_dest->dfdally_dest_terminal_id;
    assert(dest_terminal < N_TERMINALS);

    // In case we have any data to determine the average
    unsigned int const total_datapoints = data->total_msgs[dest_terminal];
    if (total_datapoints > 0) {
        double const sum_latency = data->sum_latency[dest_terminal];
        return sum_latency / total_datapoints;
    }

    // Otherwise, use "sensible" results from another simulation
    // This assumes the network is a 72 nodes 1D-DragonFly (9 groups, with 4 routers, and 2 terminals per router)
    // source and destination share the same router
    if (src_terminal / 2 == dest_terminal / 2) {
        return 2108.74;
    }
    // source and destination are in the same group
    else if (src_terminal / 8 == dest_terminal / 8) {
        return 2390.13;
    }
    // source and destination are in different groups
    else {
        return 4162.77;
    }
}

static void predict_latency_rc(struct latency_surrogate * data, tw_lp * lp) {
    (void) data;
    (void) lp;
}


struct packet_latency_predictor latency_predictor = {
    .init              = (init_pred_f) init_pred,
    .feed              = (feed_pred_f) feed_pred,
    .predict           = (predict_pred_f) predict_latency,
    .predict_rc        = (predict_pred_rc_f) predict_latency_rc,
    .predictor_data_sz = sizeof(struct latency_surrogate)
};

struct director_data my_director_data;
int ping_msg_sent_count = 0;

void director_init(struct director_data self) {
    assert(! self.is_surrogate_on());
    //self.switch_surrogate();
    //printf("Starting on %s mode\n", my_director_data.is_surrogate_on() ? "surrogate" : "vanilla");
    my_director_data = self;
}

void director_fun(tw_pe * pe) {
    //static int i = 0;
    //if (g_tw_mynode == 0) {
    //    printf(".");
    //    fflush(stdout);
    //    //printf("GVT %d at %f with snt_count=%d\n", i++, pe->GVT_sig.recv_ts, ping_msg_sent_count);
    //}

    // Do not process if the simulation ended
    if (pe->GVT_sig.recv_ts >= g_tw_ts_end) {
        return;
    }

    // Switching to and from surrogate mode at `switch_at`
    int const switch_at[] = {10};
    size_t const switch_total = sizeof(switch_at) / sizeof(switch_at[0]);
    static size_t switch_i = 0;
    if (switch_i < switch_total) {
        // Finding the "largest" ping_msg_sent_count across all PEs
        int max_msg_count = 0;
        MPI_Allreduce(&ping_msg_sent_count, &max_msg_count, 1, MPI_INT, MPI_MAX, MPI_COMM_ROSS);
        if (max_msg_count > switch_at[switch_i]) {
            //printf("\nswitching");
            my_director_data.switch_surrogate();
            //printf(" to %s\n", my_director_data.is_surrogate_on() ? "surrogate" : "vanilla");
            switch_i++;
        }
    }
}
//
// === END OF surrogate functions

static void svr_init(svr_state * s, tw_lp * lp)
{
    //Initialize State
    s->ping_msg_sent_count = 0;
    s->ping_msg_recvd_count = 0;
    s->pong_msg_sent_count = 0;
    s->pong_msg_recvd_count = 0;
    s->start_ts = 0.0;
    s->end_ts = 0.0;
    s->svr_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0); /* turns the LP Global ID into the server ID */
    s->payload_sum = 0;

    //Now we create and send a self KICKOFF message - this is a PDES coordination event and thus doesn't need to be injected into the connected network
    //so we won't use model_net_event(), that's reserved for stuff we want to send across the network

    /* Set a time from now when this message is to be received by the recipient (self in this cae.) add some tiny random noise to help avoid event ties (different events with same timestamp) */
    //the lookahead value is a value required for conservative mode execution to work, it prevents scheduling a new event within the lookahead window
    tw_stime kickoff_time = g_tw_lookahead + (tw_rand_unif(lp->rng) * .0001);

    tw_event *e;
    svr_msg *m;
    e = tw_event_new(lp->gid, kickoff_time, lp); //ROSS method to create a new event
    m = tw_event_data(e); //Gives you a pointer to the data encoded within event e
    m->svr_event_type = KICKOFF; //Set the event type so we can know how to classify the event when received
    tw_event_send(e); //ROSS method to send off the event e with the encoded data in m
}

static void handle_kickoff_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    (void) b;
    // This bit is just for testing. It allows to send a PING event only to the first LP/server
    //if (lp->gid != 0) {
    //    return;
    //}
    s->start_ts = tw_now(lp); //the time when we're starting this LP's work is NOW

    svr_msg ping_msg;

    tw_lpid local_dest = -1; //ID of a sever, relative to only servers
    tw_lpid global_dest = -1; //ID of a server LP relative to ALL LPs

    //We want to make sure we're not accidentally picking ourselves
    local_dest = tw_rand_integer(lp->rng, 1, num_nodes - 2);
    local_dest = (s->svr_id + local_dest) % num_nodes;
    //local_dest is now a number [0,num_nodes) but is assuredly not s->svr_id
    assert(local_dest >= 0);
    assert(local_dest < num_nodes);
    assert(local_dest != s->svr_id);

    ping_msg.sender_id = s->svr_id; //encode our server ID into the new ping message
    ping_msg.svr_event_type = PING; //set it to type PING
    ping_msg.payload_value = tw_rand_integer(lp->rng, 1, 10); //encode a random payload value to it from [1,10]

    codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, NULL, &rep_id, &offset); //gets information from CODES necessary to get the global LP ID of a server
    global_dest = codes_mapping_get_lpid_from_relative(local_dest, group_name, lp_type_name, NULL, 0);
    s->ping_msg_sent_count++;
    m->event_rc = model_net_event(net_id, "test", global_dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)&ping_msg, 0, NULL, lp);
}

static void handle_kickoff_rev_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    (void) b;
    model_net_event_rc2(lp, &m->event_rc); //undo any model_net_event calls encoded into this message
    s->ping_msg_sent_count--; //undo the increment of the ping_msg_sent_count in the server state
    tw_rand_reverse_unif(lp->rng); //reverse the rng call for creating a payload value;
    tw_rand_reverse_unif(lp->rng); //reverse the rng call for getting a local_dest
}

static void handle_ping_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    (void) b;
    s->ping_msg_recvd_count++; //increment the counter for ping messages received

    int original_sender = m->sender_id; //this is the server we need to send a PONG message back to
    s->payload_sum += m->payload_value; //increment our running sum of payload values received

    svr_msg pong_msg;
    pong_msg.sender_id = s->svr_id;
    pong_msg.svr_event_type = PONG;
    // only ping messages contain a payload value - not every value in a message struct must be utilized by all messages!

    codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, NULL, &rep_id, &offset); //gets information from CODES necessary to get the global LP ID of a server
    tw_lpid global_dest = codes_mapping_get_lpid_from_relative(original_sender, group_name, lp_type_name, NULL, 0);
    s->pong_msg_sent_count++;
    m->event_rc = model_net_event(net_id, "test", global_dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)&pong_msg, 0, NULL, lp);
}

static void handle_ping_rev_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    (void) b;
    model_net_event_rc2(lp, &m->event_rc); //undo any model_net_event calls encoded into this message
    s->pong_msg_sent_count--;
    s->payload_sum -= m->payload_value; //undo the increment of the payload sum
    s->ping_msg_recvd_count--; //undo the increment of the counter for ping messages received
}

static void handle_pong_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    s->pong_msg_recvd_count++; //increment the counter for ping messages received

    if(s->ping_msg_sent_count >= num_msgs) //if we've sent enough ping messages, then we stop and don't send any more
    {
        b->c1 = 1; //flag that we didn't really do anything in this event so that if this event gets reversed, we don't over-aggressively revert state or RNGs
        return;
    }

    //Now we need to send another ping message, to someone new (just to spice the simulation)
    tw_lpid send_to = tw_rand_integer(lp->rng, 1, num_nodes - 2);
    send_to = (s->svr_id + send_to) % num_nodes;

    svr_msg ping_msg;
    ping_msg.sender_id = s->svr_id; //encode our server ID into the new ping message
    ping_msg.svr_event_type = PING; //set it to type PING
    ping_msg.payload_value = tw_rand_integer(lp->rng, 1, 10); //encode a random payload value to it

    codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, NULL, &rep_id, &offset); //gets information from CODES necessary to get the global LP ID of a server
    tw_lpid global_dest = codes_mapping_get_lpid_from_relative(send_to, group_name, lp_type_name, NULL, 0);
    s->ping_msg_sent_count++;
    m->event_rc = model_net_event(net_id, "test", global_dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)&ping_msg, 0, NULL, lp);
}

static void handle_pong_rev_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    if (! b->c1) { //if we didn't flip the c1 flag in the forward event
        model_net_event_rc2(lp, &m->event_rc); //undo any model_net_event calls encoded into this message
        s->ping_msg_sent_count--;
        tw_rand_reverse_unif(lp->rng); //undo the rng for the new payload value
        tw_rand_reverse_unif(lp->rng); //undo the rng for the new server to send a ping to
        b->c1 = 0;
    }

    s->pong_msg_recvd_count--; //undo the increment of the counter for ping messages received
}

static void svr_commit(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    (void) b;
    (void) lp;

    if (s->svr_id == 0 && m->svr_event_type == PONG) {
        ping_msg_sent_count = s->ping_msg_sent_count;
    }
}

static void svr_finalize(svr_state * s, tw_lp * lp)
{
    s->end_ts = tw_now(lp);

    int total_msgs_sent = s->ping_msg_sent_count + s->pong_msg_sent_count;
    int total_msg_size_sent = PAYLOAD_SZ * total_msgs_sent;
    tw_stime time_in_seconds_sent = ns_to_s(s->end_ts - s->start_ts);

    printf("Sever LPID:%lu svr_id:%lu sent %d bytes in %f seconds, PINGs Sent: %d; PONGs Received: %d; PINGs Received: %d; PONGs Sent %d; Payload Sum: %d\n",
            (unsigned long)lp->gid, (unsigned long)s->svr_id, total_msg_size_sent,
            time_in_seconds_sent, s->ping_msg_sent_count, s->pong_msg_recvd_count, s->ping_msg_recvd_count, s->pong_msg_sent_count, s->payload_sum);
}

static void svr_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    switch (m->svr_event_type)
    {
        case KICKOFF:
            handle_kickoff_event(s, b, m, lp);
            break;
        case PING:
            handle_ping_event(s, b, m, lp);
            break;
        case PONG:
            handle_pong_event(s, b, m, lp);
            break;
        default:
            tw_error(TW_LOC, "\n Invalid message type %d ", m->svr_event_type);
            break;
    }
}

static void svr_rev_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    switch (m->svr_event_type)
    {
        case KICKOFF:
            handle_kickoff_rev_event(s, b, m, lp);
            break;
        case PING:
            handle_ping_rev_event(s, b, m, lp);
            break;
        case PONG:
            handle_pong_rev_event(s, b, m, lp);
            break;
        default:
            tw_error(TW_LOC, "\n Invalid message type %d ", m->svr_event_type);
            break;
    }
}

/* convert ns to seconds */
static tw_stime ns_to_s(tw_stime ns)
{
    return(ns / (1000.0 * 1000.0 * 1000.0));
}
static tw_stime s_to_ns(tw_stime s)
{
    return(s*1000.0*1000.0*1000.0);
}

int main(int argc, char **argv)
{
    int nprocs;
    int rank;
    int num_nets;
    int *net_ids;

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    codes_comm_update();

    //g_tw_gvt_arbitrary_fun = director_fun;
    dragonfly_dally_save_packet_latency_to_file("pingpong");
    dragonfly_dally_surrogate_configure((struct dragonfly_dally_surrogate_configure_st){
        .director_init = director_init,
        .director_call = director_fun,
        .latency_predictor = &latency_predictor
    });

    if(argc < 2)
    {
            printf("\n Usage: mpirun <args> --sync=1/2/3 -- <config_file.conf> ");
            MPI_Finalize();
            return 0;
    }

    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    MPI_Comm_size(MPI_COMM_CODES, &nprocs);

    configuration_load(argv[2], MPI_COMM_CODES, &config);

    model_net_register();
    svr_add_lp_type();

    codes_mapping_setup();

    net_ids = model_net_configure(&num_nets);
    net_id = *net_ids;
    free(net_ids);

    /* 1 day of simulation time is drastically huge but it will ensure
       that the simulation doesn't try to end before all packets are delivered */
    g_tw_ts_end = s_to_ns(24 * 60 * 60);

    num_nodes = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 1);  //get the number of nodes so we can use this value during the simulation
    assert(num_nodes);

    if(lp_io_dir[0])
    {
        do_lp_io = 1;
        int flags = lp_io_use_suffix ? LP_IO_UNIQ_SUFFIX : 0;
        int ret = lp_io_prepare(lp_io_dir, flags, &io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_prepare failure");
    }
    tw_run();
    if (do_lp_io){
        int ret = lp_io_flush(io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_flush failure");
    }
    model_net_report_stats(net_id);

    tw_end();
    return 0;
}
