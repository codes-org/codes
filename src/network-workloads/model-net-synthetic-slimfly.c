/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/*
* The test program generates some synthetic traffic patterns for the model-net network models.
* currently it only support the dragonfly network model uniform random and nearest neighbor traffic patterns.
*/

#include "codes/model-net.h"
#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"

#define PAYLOAD_SZ 256
#define LP_CONFIG_NM (model_net_lp_config_names[SLIMFLY])

#define PRINT_WORST_CASE_MATCH 0

#define PARAMS_LOG 1
FILE * slimfly_results_log_2=NULL;
FILE * slimfly_ross_csv_log=NULL;

static int net_id = 0;
static int offset = 2;
static int traffic = 1;
static double arrival_time = 1000.0;
static double load = 0.0;				//percent utilization of each terminal's uplink when sending messages
static double MEAN_INTERVAL = 0.0;
int this_packet_size = 0;
double this_global_bandwidth = 0.0;
double this_link_delay = 0;
int *worst_dest;						//Array mapping worst case destination for each router
int num_terminals;
int total_routers;

static char lp_io_dir[356] = {'\0'};
static lp_io_handle io_handle;
static unsigned int lp_io_use_suffix = 0;
static int do_lp_io = 0;

static int num_servers_per_rep = 0;
static int num_routers_per_grp = 0;
static int num_nodes_per_grp = 0;

static int num_groups = 0;
static int num_nodes = 0;

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

/* global variables for codes mapping */
static char group_name[MAX_NAME_LENGTH];
static char lp_type_name[MAX_NAME_LENGTH];
static int group_index, lp_type_index, rep_id, offset;

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
        WORST_CASE = 2,
	NEAREST_GROUP = 3, /* sends message to the node connected to the neighboring router */
	NEAREST_NEIGHBOR = 4 /* sends message to the next node (potentially connected to the same router) */
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
    model_net_event_return event_rc;
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

/***** ROSS model instrumentation *****/
void svr_vt_sample_fn(svr_state *s, tw_bf *bf, tw_lp *lp);
void svr_vt_sample_rc_fn(svr_state *s, tw_bf *bf, tw_lp *lp);
void svr_rt_sample_fn(svr_state *s, tw_lp *lp);
void svr_event_trace(svr_msg *m, tw_lp *lp, char *buffer, int *collect_flag);

char svr_name[] = "sfly_server\0";

st_model_types svr_model_types[] = {
    {svr_name,
     NULL,
     0,
     (vts_event_f) svr_vt_sample_fn,
     (vts_revent_f) svr_vt_sample_rc_fn,
     (rt_event_f) svr_rt_sample_fn,
     (ev_trace_f) svr_event_trace,
     sizeof(int)},
    {0}
};

void svr_vt_sample_fn(svr_state *s, tw_bf *bf, tw_lp *lp)
{
    (void)s;
    (void)bf;
    (void)lp;
}

void svr_vt_sample_rc_fn(svr_state *s, tw_bf *bf, tw_lp *lp)
{
    (void)s;
    (void)bf;
    (void)lp;
}

void svr_rt_sample_fn(svr_state *s, tw_lp *lp)
{
    (void)s;
    (void)lp;
}

void svr_event_trace(svr_msg *m, tw_lp *lp, char *buffer, int *collect_flag)
{
    (void)lp;
    (void)collect_flag;
    int type = (int) m->svr_event_type;
    memcpy(buffer, &type, sizeof(type));
}

static const st_model_types  *svr_get_model_stat_types(void)
{
    return(&svr_model_types[0]);
}

void svr_register_model_types()
{
    st_model_type_register("server", svr_get_model_stat_types());
}

/***** End of ROSS Instrumentation *****/

const tw_optdef app_opt [] =
{
        TWOPT_GROUP("Model net synthetic traffic " ),
        TWOPT_UINT("traffic", traffic, "UNIFORM RANDOM=1, NEAREST NEIGHBOR=2 "),
        TWOPT_STIME("arrival_time", arrival_time, "INTER-ARRIVAL TIME"),
        TWOPT_STIME("load", load, "percentage of packet inter-arrival rate to simulate"), 
        TWOPT_CHAR("lp-io-dir", lp_io_dir, "Where to place io output (unspecified -> no output"),
        TWOPT_UINT("lp-io-use-suffix", lp_io_use_suffix, "Whether to append uniq suffix to lp-io directory (default 0)"),
	TWOPT_END(),
};

const tw_lptype* svr_get_lp_type()
{
            return(&svr_lp);
}

static void svr_add_lp_type()
{
  lp_type_register("server", svr_get_lp_type());
}

/* convert GiB/s and bytes to ns */
static tw_stime bytes_to_ns(uint64_t bytes, double GB_p_s)
{
    tw_stime time;

    /* bytes to GB */
    time = ((double)bytes)/(1024.0*1024.0*1024.0);
    /* MB to s */
    time = time / GB_p_s;
    /* s to ns */
    time = time * 1000.0 * 1000.0 * 1000.0;

    return(time);
}

/** Latest implementation of function to return an array mapping each router with it's corresponding worst-case router pair
 *  @param[in/out] 	*worst_dest   		Router ID to send all messages to
 */
void init_worst_case_mapping()
{
	int i,j,k;
	int r1,r2;		//Routers to be paired
	for(k=0;k<2;k++)
	{
		for(j=0;j<num_routers_per_grp-1;j+=2)
		{
			for(i=0;i<num_routers_per_grp;i++)
			{
				r1 = i + j*num_routers_per_grp + k*num_routers_per_grp*num_routers_per_grp;
				r2 = i + (j+1)*num_routers_per_grp + k*num_routers_per_grp*num_routers_per_grp;
				worst_dest[r1] = r2;
				worst_dest[r2] = r1;
//				printf("%d->%d, %d->%d\n",r1,worst_dest[r1],r2,worst_dest[r2]);
			}
		}
	}
	j = num_routers_per_grp-1;
	for(i=0;i<num_routers_per_grp;i++)
	{
		r1 = i + j*num_routers_per_grp + 0*num_routers_per_grp*num_routers_per_grp;
		r2 = i + j*num_routers_per_grp + 1*num_routers_per_grp*num_routers_per_grp;
		worst_dest[r1] = r2;
		worst_dest[r2] = r1;
//		printf("%d->%d, %d->%d\n",r1,worst_dest[r1],r2,worst_dest[r2]);
	}
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

    configuration_get_value_int(&config, "PARAMS", "packet_size", NULL, &this_packet_size);
    if(!this_packet_size) {
        this_packet_size = 0;
        fprintf(stderr, "Packet size not specified, setting to %d\n", this_packet_size);
        exit(0);
    }

    configuration_get_value_double(&config, "PARAMS", "global_bandwidth", NULL, &this_global_bandwidth);
    if(!this_global_bandwidth) {
        this_global_bandwidth = 4.7;
        fprintf(stderr, "Bandwidth of global channels not specified, setting to %lf\n", this_global_bandwidth);
    }

/*    configuration_get_value_double(&config, "PARAMS", "link_delay", anno, &this_link_delay);
    if(!this_link_delay) {
        this_link_delay = 0;
        fprintf(stderr, "Link Delay not specified, setting to %lf\n", this_link_delay);
    }
*/
    if(arrival_time!=0)
    {
        MEAN_INTERVAL = arrival_time;
    }
    if(load != 0)
    {
        MEAN_INTERVAL = bytes_to_ns(this_packet_size, load*this_global_bandwidth);
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
    (void)lp;

    if(b->c1)
        tw_rand_reverse_unif(lp->rng);

	ns->msg_sent_count--;
	model_net_event_rc2(lp, &m->event_rc);

    tw_rand_reverse_unif(lp->rng);
}	
static void handle_kickoff_event(
	    svr_state * ns,
	    tw_bf * b,
	    svr_msg * m,
	    tw_lp * lp)
{
    (void)m;

    char anno[MAX_NAME_LENGTH];
    tw_lpid local_dest = -1, global_dest = -1;
   
    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REMOTE;

    assert(net_id == SLIMFLY); /* only supported for dragonfly model right now. */
    ns->start_ts = tw_now(lp);
//   codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, anno, &rep_id, &offset);
   codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, anno, &rep_id, &offset);
   /* in case of uniform random traffic, send to a random destination. */
   if(traffic == UNIFORM)
   {
       b->c1 = 1;
   	local_dest = tw_rand_integer(lp->rng, 0, num_nodes - 1);
//	printf("\n LP %ld sending to %d ", lp->gid, local_dest);
   }
   else if(traffic == WORST_CASE)
   {
        // Assign the global router ID
        // TODO: be annotation-aware
        int num_lps = codes_mapping_get_lp_count(group_name, 1, LP_CONFIG_NM, anno, 0);

        int src_terminal_id = (rep_id * num_lps) + offset;  
    //   s->router_id=(int)s->terminal_id / (s->params->num_routers/2);
        int src_router_id = src_terminal_id / (num_lps);
        int dst_router_id = worst_dest[src_router_id];
        //			dst_lp = tw_rand_integer(lp->rng, total_routers + num_terminals*dst_router_id, total_routers + num_terminals*(dst_router_id+1)-1);
//        local_dest = total_routers + num_terminals*dst_router_id + (src_terminal_id % num_terminals);
        local_dest = num_lps*dst_router_id + (src_terminal_id % num_terminals);
//        printf("packet path initialized. src_term:%d src_router:%d dest_router:%d dest_term:%d\n",src_terminal_id, src_router_id, dst_router_id, (int)local_dest);
   }
   else if(traffic == NEAREST_GROUP)
   {
	local_dest = (rep_id * 2 + offset + num_nodes_per_grp) % num_nodes;
//	printf("\n LP %ld sending to %ld num nodes %d ", rep_id * 2 + offset, local_dest, num_nodes);
   }	
   else if(traffic == NEAREST_NEIGHBOR)
   {
	local_dest =  (rep_id * 2 + offset + 2) % num_nodes;
//	 printf("\n LP %ld sending to %ld num nodes %d ", rep_id * 2 + offset, local_dest, num_nodes);
   }
//printf("1local_dest:%d global_dest:%d num_nodes:%d\n", local_dest, global_dest, num_nodes);
   assert(local_dest < (tw_lpid)num_nodes);
   codes_mapping_get_lp_id(group_name, lp_type_name, anno, 1, local_dest / num_servers_per_rep, local_dest % num_servers_per_rep, &global_dest);
  
//printf("2local_dest:%d global_dest:%d num_nodes:%d\n", local_dest, global_dest, num_nodes);

   ns->msg_sent_count++;
   m->event_rc = model_net_event(net_id, "test", global_dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
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

int index_mine = 0;

static void svr_finalize(
    svr_state * ns,
    tw_lp * lp)
{
    ns->end_ts = tw_now(lp);

//    printf("server %d server %llu recvd %d bytes in %f seconds, %f MiB/s sent_count %d recvd_count %d local_count %d \n", index_mine++,(unsigned long long)lp->gid, PAYLOAD_SZ*ns->msg_recvd_count, ns_to_s(ns->end_ts-ns->start_ts),
//        ((double)(PAYLOAD_SZ*ns->msg_sent_count)/(double)(1024*1024)/ns_to_s(ns->end_ts-ns->start_ts)), ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count);
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
            printf("\n Invalid message type %d ", m->svr_event_type);
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
#ifdef USE_RDAMARIS
    if(g_st_ross_rank)
    { // keep damaris ranks from running code between here up until tw_end()
#endif
    codes_comm_update();

    if(argc < 2)
    {
            printf("\n Usage: mpirun <args> --sync=2/3 mapping_file_name.conf (optional --nkp) ");
            MPI_Finalize();
            return 0;
    }

    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    MPI_Comm_size(MPI_COMM_CODES, &nprocs);

    configuration_load(argv[2], MPI_COMM_CODES, &config);
    model_net_register();
    svr_add_lp_type();
    
    if (g_st_ev_trace || g_st_model_stats || g_st_use_analysis_lps)
        svr_register_model_types();

    codes_mapping_setup();
    net_ids = model_net_configure(&num_nets);
//    assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);

    num_servers_per_rep = codes_mapping_get_lp_count("MODELNET_GRP", 1, "server", NULL, 1);
    configuration_get_value_int(&config, "PARAMS", "num_terminals", NULL, &num_terminals);
    configuration_get_value_int(&config, "PARAMS", "num_routers", NULL, &num_routers_per_grp);
    num_groups = (num_routers_per_grp * 2);
    num_nodes = num_groups * num_routers_per_grp * num_servers_per_rep;
    num_nodes_per_grp = num_routers_per_grp * num_servers_per_rep;
    total_routers = num_routers_per_grp * num_routers_per_grp * 2;

/*    if(lp_io_prepare("modelnet-test", LP_IO_UNIQ_SUFFIX, &handle, MPI_COMM_CODES) < 0)
    {
        return(-1);
    }
*/

    if(lp_io_dir[0])
    {
        do_lp_io = 1;
        int flags = lp_io_use_suffix ? LP_IO_UNIQ_SUFFIX : 0;
        int ret = lp_io_prepare(lp_io_dir, flags, &io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_prepare failure");
    }

    //WORST_CASE Initialization array
   if(traffic == WORST_CASE)
   {
        worst_dest = (int*)calloc(total_routers,sizeof(int));
        init_worst_case_mapping();
#if PRINT_WORST_CASE_MATCH
        int l;
        for(l=0; l<total_routers; l++)
        {
	        printf("match %d->%d\n",l,worst_dest[l]);
        }
#endif
   }

   tw_run();

 
   if (do_lp_io){
       int ret = lp_io_flush(io_handle, MPI_COMM_CODES);
       assert(ret == 0 || !"lp_io_flush failure");
   }

   model_net_report_stats(net_id);

    if(rank == 0)
    {
#if PARAMS_LOG
	//Open file to append simulation results
	char log[200];
	sprintf( log, "slimfly-results-log.txt");
	slimfly_results_log_2=fopen(log, "a");
	if(slimfly_results_log_2 == NULL)
		tw_error(TW_LOC, "\n Failed to open slimfly results log file \n");
	printf("Printing Simulation Parameters/Results Log File\n");
	fprintf(slimfly_results_log_2,"%16.3d, %6.2f, %13.2f, ",traffic, load, MEAN_INTERVAL);
#endif
    }

/*    if(lp_io_flush(handle, MPI_COMM_CODES) < 0)
    {
        assert(ret == 0 || !"lp_io_flush failure");
        return(-1);
    }
*/
#ifdef USE_RDAMARIS
    } // end if(g_st_ross_rank)
#endif
    tw_end();

    if(rank == 0)
    {
#if PARAMS_LOG
	slimfly_ross_csv_log=fopen("ross.csv", "r");
	if(slimfly_ross_csv_log == NULL)
		tw_error(TW_LOC, "\n Failed to open ross.csv log file \n");
	printf("Reading ROSS specific data from ross.csv and Printing to Slim Fly Log File\n");
	
	char * line = NULL;
	size_t len = 0;
	ssize_t read = getline(&line, &len, slimfly_ross_csv_log);
	while (read != -1) 
	{
		read = getline(&line, &len, slimfly_ross_csv_log);
	}
//		read = getline(&line, &len, slimfly_ross_csv_log);

	char * pch;
	pch = strtok (line,",");
	int idx = 0;
        int gvt_computations;
	long long total_events, rollbacks, net_events;
        float running_time = 0, efficiency = 0, event_rate = 0;
	while (pch != NULL)
	{
		pch = strtok (NULL, ",");
//		printf("%d: %s\n",idx,pch);
		switch(idx)
		{
/*			case 0:
				printf("%s\n",pch);
				break;
			case 1:
				printf("%s\n",pch);
				break;
			case 2:
				printf("%s\n",pch);
				break;
			case 3:
				printf("%s\n",pch);
				break;
			case 4:
				printf("%s\n",pch);
				break;
*/			
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
	fprintf(slimfly_results_log_2,"%12llu, %10llu, %16d, %10llu, %17.4f, %10.2f, %22.2f\n",total_events,rollbacks,gvt_computations,net_events,running_time,efficiency,event_rate);
	fclose(slimfly_results_log_2);
	fclose(slimfly_ross_csv_log);
#endif
    }
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
