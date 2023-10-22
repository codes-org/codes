/*
 * Copyright (C) 2019 Neil McGlohon
 * Mantained/edited by Elkin Cruz (2022-2023)
 * See LICENSE notice in top-level directory
 */

#include "codes/model-net.h"
#include "codes/codes_mapping.h"
#include "codes/surrogate/init.h"  // just needed for stats on surrogate-mode


static int net_id = 0;
static int PAYLOAD_SZ = 4096;
static int RANDOM_PAYLOAD_SZ = 0; // If turned on, it assumes that PAYLOAD_SZ is a multiple of CHUNK_SIZE
static int CHUNK_SIZE = 64; // This value depends on the network being used
static unsigned long long num_nodes = 0;

static char lp_io_dir[256] = {'\0'};
static lp_io_handle io_handle;
static unsigned int lp_io_use_suffix = 0;
static int do_lp_io = 0;

static int num_msgs = 20;
static int num_initial_msgs = 1;

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
    // Used for rollback
    tw_stime previous_ts;
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
static void svr_finalize(svr_state * s, tw_lp * lp);
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
    sizeof(svr_state),
};

const tw_optdef app_opt [] =
{
        TWOPT_GROUP("Model net synthetic traffic " ),
    	TWOPT_UINT("num_messages", num_msgs, "Number of PING messages to be generated per terminal "),
    	TWOPT_UINT("num_initial_messages", num_initial_msgs, "Number of PING messages to be injected initially at the start (larger = more congestion)"),
    	TWOPT_UINT("payload_sz",PAYLOAD_SZ, "size of the message being sent "),
    	TWOPT_UINT("random_payload_sz", RANDOM_PAYLOAD_SZ, "whether payloads are a random number between 1 and payload_sz (default 0)"),
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

    for (int i = 1; i <= num_initial_msgs && i <= num_msgs; i++) {
        tw_event *e;
        svr_msg *m;
        e = tw_event_new(lp->gid, kickoff_time * i, lp); //ROSS method to create a new event
        m = tw_event_data(e); //Gives you a pointer to the data encoded within event e
        m->svr_event_type = KICKOFF; //Set the event type so we can know how to classify the event when received
        tw_event_send(e); //ROSS method to send off the event e with the encoded data in m
    }

    s->start_ts = kickoff_time; // the time when we're starting this LP's work is when the first ping is generated
}

static void handle_kickoff_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    (void) b;
    // This bit is just for testing. It allows to send a PING event only to the first LP/server
    //if (lp->gid != 0) {
    //    return;
    //}

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
    long const payload_size = payload_size_forward(lp);
    m->event_rc = model_net_event(net_id, "test", global_dest, payload_size, 0.0, sizeof(svr_msg), (const void*)&ping_msg, 0, NULL, lp);
}

static void handle_kickoff_rev_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    (void) b;
    model_net_event_rc2(lp, &m->event_rc); //undo any model_net_event calls encoded into this message
    s->ping_msg_sent_count--; //undo the increment of the ping_msg_sent_count in the server state
    payload_size_rev(lp);
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
    long const payload_size = payload_size_forward(lp);
    m->event_rc = model_net_event(net_id, "test", global_dest, payload_size, 0.0, sizeof(svr_msg), (const void*)&pong_msg, 0, NULL, lp);
}

static void handle_ping_rev_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    (void) b;
    payload_size_rev(lp);
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
    long const payload_size = payload_size_forward(lp);
    m->event_rc = model_net_event(net_id, "test", global_dest, payload_size, 0.0, sizeof(svr_msg), (const void*)&ping_msg, 0, NULL, lp);
}

static void handle_pong_rev_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    if (! b->c1) { //if we didn't flip the c1 flag in the forward event
        model_net_event_rc2(lp, &m->event_rc); //undo any model_net_event calls encoded into this message
        s->ping_msg_sent_count--;
        payload_size_rev(lp);
        tw_rand_reverse_unif(lp->rng); //undo the rng for the new payload value
        tw_rand_reverse_unif(lp->rng); //undo the rng for the new server to send a ping to
        b->c1 = 0;
    }

    s->pong_msg_recvd_count--; //undo the increment of the counter for ping messages received
}

static void svr_finalize(svr_state * s, tw_lp * lp)
{
    int total_msgs_sent = s->ping_msg_sent_count + s->pong_msg_sent_count;
    // TODO (Elkin): this is wrong for random payload sizes
    int total_msg_size_sent = PAYLOAD_SZ * total_msgs_sent;
    tw_stime time_in_seconds_sent = ns_to_s(s->end_ts - s->start_ts);

    printf("Sever LPID:%lu svr_id:%lu sent %d bytes in %f seconds, PINGs Sent: %d; PONGs Received: %d; PINGs Received: %d; PONGs Sent %d; Payload Sum: %d\n",
            (unsigned long)lp->gid, (unsigned long)s->svr_id, total_msg_size_sent,
            time_in_seconds_sent, s->ping_msg_sent_count, s->pong_msg_recvd_count, s->ping_msg_recvd_count, s->pong_msg_sent_count, s->payload_sum);
}

static void svr_event(svr_state * s, tw_bf * b, svr_msg * m, tw_lp * lp)
{
    m->previous_ts = s->end_ts;
    s->end_ts = tw_now(lp);

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
