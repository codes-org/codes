/*
 * Copyright (C) 2019 Neil McGlohon - 2023 Elkin Cruz
 * Based on tutorial-synthetic-ping-pong.c by 2019 Neil McGlohon
 * See LICENSE notice in top-level directory
 */

#include "codes/model-net.h"
#include "codes/codes_mapping.h"
#include "codes/surrogate/init.h"  // just needed for stats on surrogate-mode


static int net_id = 0;
static int PAYLOAD_SZ = 4096;
static int RANDOM_PAYLOAD_SZ = 0; // If turned on, it assumes that PAYLOAD_SZ is a multiple of CHUNK_SIZE
static int CHUNK_SIZE = 512; // This value depends on the network configuration
static unsigned long long num_nodes = 0;

static char lp_io_dir[256] = {'\0'};
static lp_io_handle io_handle;
static unsigned int lp_io_use_suffix = 0;

static int num_msgs = 10000;
static int terminal_queue_size = 3;

/* global variables for codes mapping */
static char group_name[MAX_NAME_LENGTH];
static char lp_type_name[MAX_NAME_LENGTH];
static int group_index, lp_type_index, rep_id, offset;

/* type of events */
enum SVR_EVENT
{
    SVR_EVENT_send = 1,
    SVR_EVENT_msg
};

struct svr_msg
{
    enum SVR_EVENT svr_event_type; // kickoff, heartbeat, msg
    int sender_id; //ID of the sender workload LP to know who to send a PONG message back to
    int payload_value; //Some value that we will encode as an example
    // Used for rollback
    int payload_size; //Size of payload (the actual event is not of this size, this is just a number we decide on)
    model_net_event_return event_rc; //helper to encode data relating to CODES rng usage
    tw_stime previous_ts;
};

struct svr_state
{
    tw_lpid svr_id;       /* the ID of this server */
    int msg_sent_count;   /* messages sent */
    int msg_recvd_count;  /* messages received */
    int total_bytes_sent; /* total bytes sent */
    tw_stime start_ts;    /* time that this LP started sending requests */
    tw_stime end_ts;      /* time that this LP ended sending requests */
    int payload_sum;      /* the running sum of all payloads received */
};

/* declaration of functions */
static void svr_init(struct svr_state * s, tw_lp * lp);
static void svr_event(struct svr_state * s, tw_bf * b, struct svr_msg * m, tw_lp * lp);
static void svr_rev_event(struct svr_state * s, tw_bf * b, struct svr_msg * m, tw_lp * lp);
static void svr_finalize(struct svr_state * s, tw_lp * lp);
static tw_stime ns_to_s(tw_stime ns);
static tw_stime s_to_ns(tw_stime s);

/* ROSS lptype function callback mapping */
tw_lptype svr_lp = {
    (init_f) svr_init,
    (pre_run_f) NULL,
    (event_f) svr_event,
    (revent_f) svr_rev_event,
    (commit_f) NULL,
    (final_f)  svr_finalize,
    (map_f) codes_mapping,
    sizeof(struct svr_state),
};

const tw_optdef app_opt [] =
{
        TWOPT_GROUP("Model net synthetic traffic " ),
        TWOPT_UINT("num_messages", num_msgs, "Number of messages to be sent from terminal"),
        TWOPT_UINT("injection_queue_size", terminal_queue_size, "Number of packets in a terminal's queue at any point in time (default 2)"),
        TWOPT_UINT("payload_sz", PAYLOAD_SZ, "size of the message being sent "),
        TWOPT_UINT("random_payload_sz", RANDOM_PAYLOAD_SZ, "whether payloads are a random number between 'chunk_size' and payload_sz (default 0 -> deactivated)"),
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

static long payload_size_forward(tw_lp * lp) {
    long payload_size = PAYLOAD_SZ;
    if (RANDOM_PAYLOAD_SZ) {
        payload_size = tw_rand_integer(lp->rng, 0, PAYLOAD_SZ > CHUNK_SIZE ? PAYLOAD_SZ / CHUNK_SIZE : 1);
        payload_size *= CHUNK_SIZE;
    }
    return payload_size;
}

static void payload_size_rev(tw_lp * lp) {
    if (RANDOM_PAYLOAD_SZ) {
        tw_rand_reverse_unif(lp->rng); //reverse the rng call for creating a payload size
    }
}

static void svr_init(struct svr_state * s, tw_lp * lp)
{
    //Initialize State
    s->msg_sent_count = 0;
    s->msg_recvd_count = 0;
    s->total_bytes_sent = 0;
    s->start_ts = 0.0;
    s->end_ts = 0.0;
    s->svr_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0); /* turns the LP Global ID into the server ID */
    s->payload_sum = 0;

    // This bit is just for testing. Only the first terminal (0) sends events
    //if (lp->gid != 0) {
    //    return;
    //}

    //Now we create and send a self "kickoff" message - this is a PDES coordination event and thus doesn't need to be injected into the connected network
    //so we won't use model_net_event(), that's reserved for stuff we want to send across the network

    /* Set a time from now when this message is to be received by the recipient (self in this cae.) add some tiny random noise to help avoid event ties (different events with same timestamp) */
    //the lookahead value is a value required for conservative mode execution to work, it prevents scheduling a new event within the lookahead window
    tw_stime send_time = g_tw_lookahead + (tw_rand_unif(lp->rng) * .0001);

    for (int i = 1; i <= terminal_queue_size && i <= num_msgs; i++) {
        tw_event *e;
        struct svr_msg *m;
        e = tw_event_new(lp->gid, send_time * i, lp); //ROSS method to create a new event
        m = tw_event_data(e); //Gives you a pointer to the data encoded within event e
        m->sender_id = s->svr_id; //Set the event type so we can know how to classify the event when received
        m->svr_event_type = SVR_EVENT_send; //Set the event type so we can know how to classify the event when received
        tw_event_send(e); //ROSS method to send off the event e with the encoded data in m
    }

    s->start_ts = send_time; // the time when we're starting this LP's work is when the first ping is generated
}

static void handle_send_event(struct svr_state * s, tw_bf * b, struct svr_msg * m, tw_lp * lp)
{
    (void) b;

    if(s->msg_sent_count >= num_msgs) {//if we've sent enough messages, then we stop and don't send any more
        b->c1 = 1; //flag that we didn't really do anything in this event so that if this event gets reversed, we don't over-aggressively revert state or RNGs
        return;
    }
    assert((tw_lpid) m->sender_id == s->svr_id);

    tw_lpid local_dest = -1; //ID of a sever, relative to only servers
    tw_lpid global_dest = -1; //ID of a server LP relative to ALL LPs

    //We want to make sure we're not accidentally picking ourselves
    local_dest = tw_rand_integer(lp->rng, 1, num_nodes - 2);
    local_dest = (s->svr_id + local_dest) % num_nodes;
    //local_dest is now a number [0,num_nodes) but is assuredly not s->svr_id
    assert(local_dest >= 0);
    assert(local_dest < num_nodes);
    assert(local_dest != s->svr_id);

    // Message to send to random terminal
    struct svr_msg msg_to_send;
    msg_to_send.sender_id = s->svr_id; //encode our server ID into the new ping message
    msg_to_send.svr_event_type = SVR_EVENT_msg; //set it to type MSG
    msg_to_send.payload_value = tw_rand_integer(lp->rng, 1, 10); //encode a random payload value to it from [1,10]
    long const payload_size = payload_size_forward(lp);
    m->payload_size = payload_size;
    s->total_bytes_sent += payload_size;

    // Message to send to self, in order to inject more another packet
    struct svr_msg msg_to_self;
    msg_to_self.sender_id = s->svr_id;
    msg_to_self.svr_event_type = SVR_EVENT_send; // when the packet finally leaves the terminal, this event will be sent back to us

    codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, NULL, &rep_id, &offset); //gets information from CODES necessary to get the global LP ID of a server
    global_dest = codes_mapping_get_lpid_from_relative(local_dest, group_name, lp_type_name, NULL, 0);
    s->msg_sent_count++;
    m->event_rc = model_net_event(
            net_id, "test", global_dest, payload_size, 0.0,
            sizeof(struct svr_msg), (const void*)&msg_to_send,
            sizeof(struct svr_msg), (const void*)&msg_to_self, lp);
}

static void handle_send_rev_event(struct svr_state * s, tw_bf * b, struct svr_msg * m, tw_lp * lp)
{
    (void) b;
    if (! b->c1) { //if we didn't flip the c1 flag in the forward event
        model_net_event_rc2(lp, &m->event_rc); //undo any model_net_event calls encoded into this message
        s->msg_sent_count--; //undo the increment of the ping_msg_sent_count in the server state
        s->total_bytes_sent -= m->payload_size;
        payload_size_rev(lp);
        tw_rand_reverse_unif(lp->rng); //reverse the rng call for creating a payload value;
        tw_rand_reverse_unif(lp->rng); //reverse the rng call for getting a local_dest
        b->c1 = 0;
    }
}

static void handle_recv_event(struct svr_state * s, tw_bf * b, struct svr_msg * m, tw_lp * lp)
{
    (void) b;
    (void) lp;
    s->msg_recvd_count++; //increment the counter for ping messages received
    s->payload_sum += m->payload_value; //increment our running sum of payload values received
}

static void handle_recv_rev_event(struct svr_state * s, tw_bf * b, struct svr_msg * m, tw_lp * lp)
{
    (void) b;
    (void) lp;
    s->payload_sum -= m->payload_value; //undo the increment of the payload sum
    s->msg_recvd_count--; //undo the increment of the counter for ping messages received
}

static void svr_finalize(struct svr_state * s, tw_lp * lp)
{
    tw_stime time_in_seconds_sent = ns_to_s(s->end_ts - s->start_ts);

    printf("Server LPID:%lu svr_id:%lu sent %d bytes in %f seconds, MSGs Sent: %d; MSGs Received: %d Payload Sum: %d\n",
            (unsigned long)lp->gid, (unsigned long)s->svr_id, s->total_bytes_sent,
            time_in_seconds_sent, s->msg_sent_count, s->msg_recvd_count, s->payload_sum);
}

static void svr_event(struct svr_state * s, tw_bf * b, struct svr_msg * m, tw_lp * lp)
{
    m->previous_ts = s->end_ts;
    s->end_ts = tw_now(lp);

    switch (m->svr_event_type)
    {
        case SVR_EVENT_send:
            handle_send_event(s, b, m, lp);
            break;
        case SVR_EVENT_msg:
            handle_recv_event(s, b, m, lp);
            break;
        default:
            tw_error(TW_LOC, "\n Invalid message type %d ", m->svr_event_type);
            break;
    }
}

static void svr_rev_event(struct svr_state * s, tw_bf * b, struct svr_msg * m, tw_lp * lp)
{
    switch (m->svr_event_type)
    {
        case SVR_EVENT_send:
            handle_send_rev_event(s, b, m, lp);
            break;
        case SVR_EVENT_msg:
            handle_recv_rev_event(s, b, m, lp);
            break;
        default:
            tw_error(TW_LOC, "\n Invalid message type %d ", m->svr_event_type);
            break;
    }

    s->end_ts = m->previous_ts;
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

    /* 1 day of simulation time is drastically huge but it will ensure
       that the simulation doesn't try to end before all packets are delivered */
    g_tw_ts_end = s_to_ns(24 * 60 * 60);

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    codes_comm_update();

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

    num_nodes = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 1);  //get the number of nodes so we can use this value during the simulation
    assert(num_nodes);

    int rc = configuration_get_value_int(&config, "PARAMS", "chunk_size", NULL, &CHUNK_SIZE);
    if(rc) { CHUNK_SIZE = 512; }

    bool do_lp_io = 0;
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

    // Printing some stats
    print_surrogate_stats();

    tw_end();
    return 0;
}

