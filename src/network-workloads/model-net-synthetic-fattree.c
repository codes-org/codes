/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/*
* The test program generates some synthetic traffic patterns for the model-net network models.
* currently it only support the fat tree network model uniform random and nearest neighbor traffic patterns.
*/

#include "codes/model-net.h"
#include "codes/lp-io.h"
#include "codes/net/fattree.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"

#define PAYLOAD_SZ 512

#define PARAMS_LOG 1

static int net_id = 0;
static int offset = 2;
static int traffic = 1;
static double arrival_time = 1000.0;
static double load = 0.0;	//Percent utilization of terminal uplink
static double MEAN_INTERVAL = 0.0;
/* whether to pull instead of push */

static int num_servers_per_rep = 0;

static int num_nodes = 0;
static int switch_radix = 0;
static int num_pods = 0;
static int num_levels = 0;
static int reps_per_pod = 0;
static int lps_per_rep = 0;

static char lp_io_dir[256] = {'\0'};
static lp_io_handle io_handle;
static unsigned int lp_io_use_suffix = 0;
static int do_lp_io = 0;

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

/* global variables for codes mapping */
static char group_name[MAX_NAME_LENGTH];
static char lp_type_name[MAX_NAME_LENGTH];
static int group_index, lp_type_index, rep_id, offset;

/* convert GiB/s and bytes to ns */
 static tw_stime bytes_to_ns(uint64_t bytes, double GB_p_s)
 {
     tw_stime time;
 
     /* bytes to GB */
     time = ((double)bytes)/(1024.0*1024.0*1024.0);
     /* GiB to s */
     time = time / GB_p_s;
     /* s to ns */
     time = time * 1000.0 * 1000.0 * 1000.0;
 
     return(time);
 }

/* type of events */
enum svr_event
{
    KICKOFF,	   /* kickoff event */
    REMOTE,        /* remote event */
    LOCAL      /* local event */
};

/* type of synthetic traffic */
enum TRAFFIC
{
	UNIFORM = 1, /* sends message to a randomly selected node */
	NEAREST_POD = 2, /* sends message to the similar offset node connected to the next pod */
        ONE_TO_ONE = 3, /* sends message in one-to-one mapping of nodes */
};

struct svr_state
{
    int msg_sent_count;   /* requests sent */
    int msg_recvd_count;  /* requests recvd */
    int local_recvd_count; /* number of local messages received */
    tw_stime start_ts;    /* time that we started sending requests */
    tw_stime end_ts;      /* time that we ended sending requests */
};

struct svr_msg
{
    enum svr_event svr_event_type;
    tw_lpid src;          /* source of this request or ack */
    int incremented_flag; /* helper for reverse computation */
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
	TWOPT_UINT("traffic", traffic, "UNIFORM RANDOM=1, NEAREST POD=2 , ONE TO ONE=3"),
	TWOPT_STIME("arrival_time", arrival_time, "INTER-ARRIVAL TIME"),
        TWOPT_STIME("load", load, "percentage of terminal link bandiwdth to inject packets"),
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
  lp_type_register("server", svr_get_lp_type());
}

static void issue_event(
    svr_state * ns,
    tw_lp * lp)
{
    (void)ns;
    tw_event *e;
    svr_msg *m;
    tw_stime kickoff_time;

    /* each server sends a dummy event to itself that will kick off the real
     * simulation
     */

    int this_packet_size = 0;
    double this_link_bandwidth = 0.0;

    configuration_get_value_int(&config, "PARAMS", "packet_size", NULL, &this_packet_size);
    if(!this_packet_size) {
        this_packet_size = 0;
        fprintf(stderr, "Packet size not specified, setting to %d\n", this_packet_size);
        exit(0);
    }

    PAYLOAD_SZ = this_packet_size;

    configuration_get_value_double(&config, "PARAMS", "link_bandwidth", NULL, &this_link_bandwidth);
    if(!this_link_bandwidth) {
        this_link_bandwidth = 4.7;
        fprintf(stderr, "Bandwidth of channels not specified, setting to %lf\n", this_link_bandwidth);
    }

    if(arrival_time!=0)
    {
        MEAN_INTERVAL = arrival_time;
    }
    if(load != 0)
    {
        MEAN_INTERVAL = bytes_to_ns(this_packet_size, load*this_link_bandwidth);
    }

    /* skew each kickoff event slightly to help avoid event ties later on */
    kickoff_time = g_tw_lookahead + tw_rand_exponential(lp->rng, MEAN_INTERVAL);

    e = tw_event_new(lp->gid, kickoff_time, lp);
    m = tw_event_data(e);
    m->svr_event_type = KICKOFF;
    tw_event_send(e);
}

static void svr_init(
    svr_state * ns,
    tw_lp * lp)
{
    ns->start_ts = 0.0;

    issue_event(ns, lp);
    return;
}

static void handle_kickoff_rev_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
    (void)b;
    (void)m;

    ns->msg_sent_count--;
    model_net_event_rc(net_id, lp, PAYLOAD_SZ);

    if(traffic == UNIFORM)
    {
        tw_rand_reverse_unif(lp->rng);
    }
}
static void handle_kickoff_event(
	    svr_state * ns,
	    tw_bf * b,
	    svr_msg * m,
	    tw_lp * lp)
{
    (void)b;
    (void)m;

    char anno[MAX_NAME_LENGTH];
    tw_lpid local_dest = -1, global_dest = -1;
   
    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REMOTE;

    ns->start_ts = tw_now(lp);

   codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, anno, &rep_id, &offset);
   tw_lpid src_lid = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
   /* in case of uniform random traffic, send to a random destination. */
   if(traffic == UNIFORM)
   {
   	local_dest = tw_rand_integer(lp->rng, 0, num_nodes - 1);
   }
   if(traffic == NEAREST_POD)
   {
      local_dest = (src_lid + reps_per_pod * lps_per_rep) % num_nodes;
   }
   if(traffic == ONE_TO_ONE)
   {
      local_dest = num_nodes - src_lid - 1;
   }

   assert(local_dest < LLU(num_nodes));

   global_dest = codes_mapping_get_lpid_from_relative(local_dest, group_name, lp_type_name, NULL, 0);

   // If Destination is self, then generate new destination
   if((int)global_dest == (int)lp->gid)
   {
       local_dest = (local_dest+1) % (num_nodes-1);
       global_dest = codes_mapping_get_lpid_from_relative(local_dest, group_name, lp_type_name, NULL, 0);
   }

   ns->msg_sent_count++;

   model_net_event(net_id, "test", global_dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);

   issue_event(ns, lp);
   return;
}

static void handle_remote_rev_event(
            svr_state * ns,     
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
        (void)b;
        (void)m;
        (void)lp;
        ns->msg_recvd_count--;
}

static void handle_remote_event(
	    svr_state * ns,
	    tw_bf * b,
	    svr_msg * m,
	    tw_lp * lp)
{
    (void)b;
    (void)m;
    (void)lp;
	ns->msg_recvd_count++;
}

static void handle_local_rev_event(
                svr_state * ns,
                tw_bf * b,
                svr_msg * m,
                tw_lp * lp)
{
    (void)b;
    (void)m;
    (void)lp;
	ns->local_recvd_count--;
}

static void handle_local_event(
                svr_state * ns,
                tw_bf * b,
                svr_msg * m,
                tw_lp * lp)
{
    (void)b;
    (void)m;
    (void)lp;
    ns->local_recvd_count++;
}

static void svr_finalize(
    svr_state * ns,
    tw_lp * lp)
{
    ns->end_ts = tw_now(lp);

    return;
}

static void svr_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    switch (m->svr_event_type)
    {
	case REMOTE:
		handle_remote_rev_event(ns, b, m, lp);
		break;
	case LOCAL:
		handle_local_rev_event(ns, b, m, lp);
		break;
	case KICKOFF:
		handle_kickoff_rev_event(ns, b, m, lp);
		break;
	default:
		assert(0);
		break;
    }
}

static void svr_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
   switch (m->svr_event_type)
    {
        case REMOTE:
            handle_remote_event(ns, b, m, lp);
            break;
        case LOCAL:
            handle_local_event(ns, b, m, lp);
            break;
	case KICKOFF:
	    handle_kickoff_event(ns, b, m, lp);
	    break;
        default:
            printf("\n LP: %d has received invalid message from src lpID: %d of message type:%d", (int)lp->gid, (int)m->src, m->svr_event_type);
            assert(0);
        break;
    }
}

int main(
    int argc,
    char **argv)
{

    int nprocs;
    int rank;
    int num_nets;
    int *net_ids;

    tw_opt_add(app_opt);

    tw_init(&argc, &argv);

    offset = 1;

    if(argc < 2)
    {
            printf("\n Usage: mpirun <args> --sync=2/3 mapping_file_name.conf (optional --nkp) ");
            MPI_Finalize();
            return 0;
    }

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    configuration_load(argv[2], MPI_COMM_WORLD, &config);

    model_net_register();

    svr_add_lp_type();

    codes_mapping_setup();

    net_ids = model_net_configure(&num_nets);
    //assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);

    if(net_id != FATTREE)
    {
	    printf("\n The test works with fat tree model configuration only! ");
        MPI_Finalize();
        return 0;
    }
    num_servers_per_rep = codes_mapping_get_lp_count("MODELNET_GRP", 1, "server",
            NULL, 1);
    
    configuration_get_value_int(&config, "PARAMS", "switch_radix", NULL, &switch_radix);
    configuration_get_value_int(&config, "PARAMS", "num_levels", NULL, &num_levels);
    num_pods  = codes_mapping_get_group_reps("MODELNET_GRP") / (switch_radix/2);
    num_nodes = codes_mapping_get_lp_count("MODELNET_GRP", 0, "server", NULL, 1);
    reps_per_pod = switch_radix / 2;
    lps_per_rep = switch_radix + num_levels;

    printf("num_nodes:%d \n",num_nodes);
    printf("num_pods:%d \n",num_pods);
    printf("reps_per_pod:%d \n",reps_per_pod);
    printf("lps_per_rep:%d \n",lps_per_rep);

    if(lp_io_dir[0])
    {
        do_lp_io = 1;
        int flags = lp_io_use_suffix ? LP_IO_UNIQ_SUFFIX : 0;
        int ret = lp_io_prepare(lp_io_dir, flags, &io_handle, MPI_COMM_WORLD);
        assert(ret == 0 || !"lp_io_prepare failure");
    }

    g_lp_io_dir = lp_io_handle_to_dir(io_handle);
    tw_run();

    if (do_lp_io){
        int ret = lp_io_flush(io_handle, MPI_COMM_WORLD);
        assert(ret == 0 || !"lp_io_flush failure");
    }

    model_net_report_stats(net_id);

#if PARAMS_LOG
    if(!g_tw_mynode)
    {
	char temp_filename[1024];
	char temp_filename_header[1024];
	sprintf(temp_filename,"%s/sim_log",g_lp_io_dir);
	sprintf(temp_filename_header,"%s/sim_log_header",g_lp_io_dir);
	FILE *fattree_results_log=fopen(temp_filename, "a");
	FILE *fattree_results_log_header=fopen(temp_filename_header, "a");
	if(fattree_results_log == NULL)
		printf("\n Failed to open results log file %s in synthetic-fattree\n",temp_filename);
	if(fattree_results_log_header == NULL)
		printf("\n Failed to open results log header file %s in synthetic-fattree\n",temp_filename_header);
	printf("Printing Simulation Parameters/Results Log File\n");
	fprintf(fattree_results_log_header,"<Workload>, <Load>, <Mean Interval>, ");
	fprintf(fattree_results_log,"%11.3d, %5.2f, %15.2f, ",traffic, load, MEAN_INTERVAL);
	fclose(fattree_results_log_header);
	fclose(fattree_results_log);
    }
#endif
    tw_end();

#if PARAMS_LOG
    if(!g_tw_mynode)
    {
	char temp_filename[1024];
	char temp_filename_header[1024];
	sprintf(temp_filename,"%s/sim_log",g_lp_io_dir);
	sprintf(temp_filename_header,"%s/sim_log_header",g_lp_io_dir);
	FILE *fattree_results_log=fopen(temp_filename, "a");
	FILE *fattree_results_log_header=fopen(temp_filename_header, "a");
	FILE *fattree_ross_csv_log=fopen("ross.csv", "r");
	if(fattree_results_log == NULL)
		printf("\n Failed to open results log file %s in synthetic-fattree\n",temp_filename);
	if(fattree_results_log_header == NULL)
		printf("\n Failed to open results log header file %s in synthetic-fattree\n",temp_filename_header);
	if(fattree_ross_csv_log == NULL)
		tw_error(TW_LOC, "\n Failed to open ross.csv log file \n");
	printf("Reading ROSS specific data from ross.csv and Printing to Fat Tree Log File\n");
	char * line = NULL;
	size_t len = 0;
	ssize_t read = getline(&line, &len, fattree_ross_csv_log);
	while (read != -1) 
	{
		read = getline(&line, &len, fattree_ross_csv_log);
	}

	char * pch;
	pch = strtok (line,",");
	int idx = 0;
        int gvt_computations;
	long long total_events, rollbacks, net_events;
        float running_time, efficiency, event_rate;
	while (pch != NULL)
	{
		pch = strtok (NULL, ",");
		switch(idx)
		{
			case 4:
				total_events = atoll(pch);
				break;
			case 13:
				rollbacks = atoll(pch);
				break;
			case 17:
				gvt_computations = atoi(pch);
				break;
			case 18:
				net_events = atoll(pch);
				break;
			case 3:
				running_time = atof(pch);
				break;
			case 8:
				efficiency = atof(pch);
				break;
			case 19:
				event_rate = atof(pch);
				break;
		}
		idx++;
	}
	fprintf(fattree_results_log_header,"<Total Events>, <Rollbacks>, <GVT Computations>, <Net Events>, <Running Time>, <Efficiency>, <Event Rate>, ");
	fprintf(fattree_results_log,"%14llu, %11llu, %18d, %12llu, %14.4f, %12.2f, %12.2f, ",total_events,rollbacks,gvt_computations,net_events,running_time,efficiency,event_rate);
	fclose(fattree_results_log);
	fclose(fattree_results_log_header);
	fclose(fattree_ross_csv_log);
    }
#endif

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
