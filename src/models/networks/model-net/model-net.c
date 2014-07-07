/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <string.h>
#include <assert.h>

#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/model-net-sched.h"
#include "codes/codes.h"

#define STR_SIZE 16
#define PROC_TIME 10.0

extern struct model_net_method simplenet_method;
extern struct model_net_method simplewan_method;
extern struct model_net_method torus_method;
extern struct model_net_method dragonfly_method;
extern struct model_net_method loggp_method;

#define X(a,b,c,d) b,
char * model_net_lp_config_names[] = {
    NETWORK_DEF
};
#undef X

#define X(a,b,c,d) c,
char * model_net_method_names[] = {
    NETWORK_DEF
};
#undef X

/* Global array initialization, terminated with a NULL entry */
#define X(a,b,c,d) d,
struct model_net_method* method_array[] = { 
    NETWORK_DEF
};
#undef X

int in_sequence = 0;
tw_stime mn_msg_offset = 0.0;

int model_net_setup(char* name,
		    uint64_t packet_size,
		    const void* net_params)
{
     int i;
    /* find struct for underlying method (according to configuration file) */
     for(i=0; method_array[i] != NULL; i++)
     {
     	if(strcmp(model_net_method_names[i], name) == 0)
	{
	   method_array[i]->mn_setup(net_params);
	   method_array[i]->packet_size = packet_size;
	   return(i);
	}
     }
     fprintf(stderr, "Error: undefined network name %s (Available options simplenet, torus, dragonfly) \n", name);
     return -1; // indicating error
}

int model_net_get_id(char *name){
    int i;
    for(i=0; method_array[i] != NULL; i++) {
        if(strcmp(model_net_method_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

void model_net_write_stats(tw_lpid lpid, struct mn_stats* stat)
{
    int ret;
    char id[19+CATEGORY_NAME_MAX+1];
    char data[1024];

    sprintf(id, "model-net-category-%s", stat->category);
    sprintf(data, "lp:%ld\tsend_count:%ld\tsend_bytes:%ld\tsend_time:%f\t" 
        "recv_count:%ld\trecv_bytes:%ld\trecv_time:%f\tmax_event_size:%ld\n",
        (long)lpid,
        stat->send_count,
        stat->send_bytes,
        stat->send_time,
        stat->recv_count,
        stat->recv_bytes,
        stat->recv_time,
        stat->max_event_size);

    ret = lp_io_write(lpid, id, strlen(data), data);
    assert(ret == 0);

    return;
}

void model_net_print_stats(tw_lpid lpid, mn_stats mn_stats_array[])
{

    int i;
    struct mn_stats all;

    memset(&all, 0, sizeof(all));
    sprintf(all.category, "all");

    for(i=0; i<CATEGORY_MAX; i++)
    {
        if(strlen(mn_stats_array[i].category) > 0)
        {
            all.send_count += mn_stats_array[i].send_count;
            all.send_bytes += mn_stats_array[i].send_bytes;
            all.send_time += mn_stats_array[i].send_time;
            all.recv_count += mn_stats_array[i].recv_count;
            all.recv_bytes += mn_stats_array[i].recv_bytes;
            all.recv_time += mn_stats_array[i].recv_time;
            if(mn_stats_array[i].max_event_size > all.max_event_size)
                all.max_event_size = mn_stats_array[i].max_event_size;

            model_net_write_stats(lpid, &mn_stats_array[i]);
        }
    }
    model_net_write_stats(lpid, &all);
}

struct mn_stats* model_net_find_stats(const char* category, mn_stats mn_stats_array[])
{
    int i;
    int new_flag = 0;
    int found_flag = 0;

    for(i=0; i<CATEGORY_MAX; i++)
    {
        if(strlen(mn_stats_array[i].category) == 0)
        {
            found_flag = 1;
            new_flag = 1;
            break;
        }
        if(strcmp(category, mn_stats_array[i].category) == 0)
        {
            found_flag = 1;
            new_flag = 0;
            break;
        }
    }
    assert(found_flag);

    if(new_flag)
    {
        strcpy(mn_stats_array[i].category, category);
    }
    return(&mn_stats_array[i]);
}

static void model_net_event_impl_base(
        int net_id,
        char* category, 
        tw_lpid final_dest_lp, 
        uint64_t message_size, 
        int is_pull,
        tw_stime offset,
        int remote_event_size,
        const void* remote_event,
        int self_event_size,
        const void* self_event,
        tw_lp *sender) {

    if (remote_event_size + self_event_size + sizeof(model_net_wrap_msg) 
            > g_tw_msg_sz){
        tw_error(TW_LOC, "Error: model_net trying to transmit an event of size "
                         "%d but ROSS is configured for events of size %zd\n",
                         remote_event_size+self_event_size+sizeof(model_net_wrap_msg),
                         g_tw_msg_sz);
        return;
    }

    tw_lpid mn_lp = model_net_find_local_device(net_id, sender);
    tw_stime poffset = codes_local_latency(sender);
    if (in_sequence){
        tw_stime tmp = mn_msg_offset;
        mn_msg_offset += poffset;
        poffset += tmp;
    }
    tw_event *e = codes_event_new(mn_lp, poffset+offset, sender);

    model_net_wrap_msg *m = tw_event_data(e);
    m->event_type = MN_BASE_NEW_MSG;
    m->magic = model_net_base_magic;

    // set the request struct 
    model_net_request *r = &m->msg.m_base.u.req;
    r->net_id = net_id;
    r->packet_size = model_net_get_packet_size(net_id);
    r->final_dest_lp = final_dest_lp;
    r->src_lp = sender->gid;
    r->msg_size = message_size;
    r->remote_event_size = remote_event_size;
    r->self_event_size = self_event_size;
    r->is_pull = is_pull;
    strncpy(r->category, category, CATEGORY_NAME_MAX-1);
    r->category[CATEGORY_NAME_MAX-1]='\0';
    
    void *e_msg = (m+1);
    if (remote_event_size > 0){
        memcpy(e_msg, remote_event, remote_event_size);
        e_msg = (char*)e_msg + remote_event_size; 
    }
    if (self_event_size > 0){
        memcpy(e_msg, self_event, self_event_size);
    }

    //print_base(m);
    tw_event_send(e);
}
static void model_net_event_impl_base_rc(tw_lp *sender){
    codes_local_latency_reverse(sender);
}

void model_net_event(
    int net_id,
    char* category, 
    tw_lpid final_dest_lp, 
    uint64_t message_size, 
    tw_stime offset,
    int remote_event_size,
    const void* remote_event,
    int self_event_size,
    const void* self_event,
    tw_lp *sender)
{
    model_net_event_impl_base(net_id, category, final_dest_lp, message_size,
            0, offset, remote_event_size, remote_event, self_event_size,
            self_event, sender);
}

void model_net_pull_event(
        int net_id,
        char *category,
        tw_lpid final_dest_lp,
        uint64_t message_size,
        tw_stime offset,
        int self_event_size,
        const void *self_event,
        tw_lp *sender){
    /* NOTE: for a pull, we are filling the *remote* event - it will be remote
     * from the destination's POV */
    model_net_event_impl_base(net_id, category, final_dest_lp, message_size,
            1, offset, self_event_size, self_event, 0, NULL, sender);
}


int model_net_set_params()
{
  char mn_name[MAX_NAME_LENGTH];
  char sched[MAX_NAME_LENGTH];
  long int packet_size_l = 0;
  uint64_t packet_size;
  int net_id=-1;
  int ret;

  config_lpgroups_t paramconf;
  configuration_get_lpgroups(&config, "PARAMS", &paramconf);
  configuration_get_value(&config, "PARAMS", "modelnet", mn_name, MAX_NAME_LENGTH);
  ret = configuration_get_value(&config, "PARAMS", "modelnet_scheduler", sched,
          MAX_NAME_LENGTH);

  configuration_get_value_longint(&config, "PARAMS", "packet_size", &packet_size_l);
  packet_size = packet_size_l;

    if (ret > 0){
        int i;
        for (i = 0; i < MAX_SCHEDS; i++){
            if (strcmp(sched_names[i], sched) == 0){
                mn_sched_type = i;
                break;
            }
        }
        if (i == MAX_SCHEDS){
            fprintf(stderr, 
                    "Unknown value for PARAMS:modelnet-scheduler : %s\n", 
                    sched);
            abort();
        }
    }
    else{
        // default: FCFS
        mn_sched_type = MN_SCHED_FCFS;
    }

    if (mn_sched_type == MN_SCHED_FCFS_FULL){
        // override packet size to something huge (leave a bit in the unlikely
        // case that an op using packet size causes overflow)
        packet_size = 1ull << 62;
    }
    else if (!packet_size && mn_sched_type != MN_SCHED_FCFS_FULL)
    {
        packet_size = 512;
        fprintf(stderr, "\n Warning, no packet size specified, setting packet size to %llu ", packet_size);
    }

  if(strcmp(model_net_method_names[SIMPLENET],mn_name)==0)
   {
     double net_startup_ns, net_bw_mbps;
     simplenet_param net_params;
     
     configuration_get_value_double(&config, "PARAMS", "net_startup_ns", &net_startup_ns);
     configuration_get_value_double(&config, "PARAMS", "net_bw_mbps", &net_bw_mbps);
     net_params.net_startup_ns = net_startup_ns;
     net_params.net_bw_mbps =  net_bw_mbps;
     net_id = model_net_setup(model_net_method_names[SIMPLENET], packet_size, (const void*)&net_params); /* Sets the network as simplenet and packet size 512 */
   }
  else if (strcmp(model_net_method_names[SIMPLEWAN],mn_name)==0){
    simplewan_param net_params;
    configuration_get_value_relpath(&config, "PARAMS", "net_startup_ns_file", net_params.startup_filename, MAX_NAME_LENGTH);
    configuration_get_value_relpath(&config, "PARAMS", "net_bw_mbps_file", net_params.bw_filename, MAX_NAME_LENGTH);
    net_id = model_net_setup(model_net_method_names[SIMPLEWAN], packet_size, (const void*)&net_params);
  }
   else if(strcmp(model_net_method_names[LOGGP],mn_name)==0)
   {
     char net_config_file[256];
     loggp_param net_params;
     
     configuration_get_value_relpath(&config, "PARAMS", "net_config_file", net_config_file, 256);
     net_params.net_config_file = net_config_file;
     net_id = model_net_setup(model_net_method_names[LOGGP], packet_size, (const void*)&net_params); /* Sets the network as loggp and packet size 512 */
   }

  else if(strcmp(model_net_method_names[DRAGONFLY], mn_name)==0)	  
    {
       dragonfly_param net_params;
       int num_routers=0, num_vcs=0, local_vc_size=0, global_vc_size=0, cn_vc_size=0, chunk_size=0;
       double local_bandwidth=0.0, cn_bandwidth=0.0, global_bandwidth=0.0;
       
       configuration_get_value_int(&config, "PARAMS", "num_routers", &num_routers);
       if(!num_routers)
	{
	   num_routers = 4; 
	   printf("\n Number of dimensions not specified, setting to %d ", num_routers);
        } 
       net_params.num_routers = num_routers; 

       configuration_get_value_int(&config, "PARAMS", "num_vcs", &num_vcs);
       if(!num_vcs)
       {
          num_vcs = 1;
	  printf("\n Number of virtual channels not specified, setting to %d ", num_vcs);
       }
       net_params.num_vcs = num_vcs;

       configuration_get_value_int(&config, "PARAMS", "local_vc_size", &local_vc_size);
       if(!local_vc_size)
	{
	   local_vc_size = 1024;
	   printf("\n Buffer size of local channels not specified, setting to %d ", local_vc_size);
	}
       net_params.local_vc_size = local_vc_size;

       configuration_get_value_int(&config, "PARAMS", "global_vc_size", &global_vc_size);
       if(!global_vc_size)
	{
	  global_vc_size = 2048;
	  printf("\n Buffer size of global channels not specified, setting to %d ", global_vc_size);
	}
       net_params.global_vc_size = global_vc_size;

       configuration_get_value_int(&config, "PARAMS", "cn_vc_size", &cn_vc_size);
       if(!cn_vc_size)
	 {
	    cn_vc_size = 1024;
	    printf("\n Buffer size of compute node channels not specified, setting to %d ", cn_vc_size);
	 }
       net_params.cn_vc_size = cn_vc_size;

	configuration_get_value_int(&config, "PARAMS", "chunk_size", &chunk_size);
	if(!chunk_size)
	  {
		chunk_size = 64;
		printf("\n Chunk size for packets is specified, setting to %d ", chunk_size);
	  }
	net_params.chunk_size = chunk_size;

	configuration_get_value_double(&config, "PARAMS", "local_bandwidth", &local_bandwidth);
        if(!local_bandwidth)
	  {
	    local_bandwidth = 5.25;
	    printf("\n Bandwidth of local channels not specified, setting to %lf ", local_bandwidth);
	 }
       net_params.local_bandwidth = local_bandwidth;

       configuration_get_value_double(&config, "PARAMS", "global_bandwidth", &global_bandwidth);
        if(!global_bandwidth)
	{
	     global_bandwidth = 4.7;
	     printf("\n Bandwidth of global channels not specified, setting to %lf ", global_bandwidth);
	}
	net_params.global_bandwidth = global_bandwidth;

	configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", &cn_bandwidth);
	if(!cn_bandwidth)
	 {
	     cn_bandwidth = 5.25;
	     printf("\n Bandwidth of compute node channels not specified, setting to %lf ", cn_bandwidth);
	}
	net_params.cn_bandwidth = cn_bandwidth;

	
       char routing[MAX_NAME_LENGTH];
       configuration_get_value(&config, "PARAMS", "routing", routing, MAX_NAME_LENGTH);
       if(strcmp(routing, "minimal") == 0)
	   net_params.routing = 0;
       else if(strcmp(routing, "nonminimal")==0 || strcmp(routing,"non-minimal")==0)
	       net_params.routing = 1;
	else if (strcmp(routing, "adaptive") == 0)
		net_params.routing = 2;
       else
       {
       	   printf("\n No routing protocol specified, setting to minimal routing");
   	   net_params.routing = 0;	   
       }
    net_id = model_net_setup(model_net_method_names[DRAGONFLY], packet_size, (const void*)&net_params);   
    }
   else if(strcmp(model_net_method_names[TORUS], mn_name)==0)
     {
	torus_param net_params;
	char dim_length[MAX_NAME_LENGTH];
	int n_dims=0, buffer_size=0, num_vc=0, i=0, chunk_size = 0;
	double link_bandwidth=0;

	configuration_get_value_int(&config, "PARAMS", "n_dims", &n_dims);
	if(!n_dims)
	{
	   n_dims = 4; /* a 4-D torus */
	   printf("\n Number of dimensions not specified, setting to %d ", n_dims);
	}
	
	configuration_get_value_double(&config, "PARAMS", "link_bandwidth", &link_bandwidth);	
	if(!link_bandwidth)
	{
		link_bandwidth = 2.0; /*default bg/q configuration */
		printf("\n Link bandwidth not specified, setting to %lf ", link_bandwidth);
	}

	configuration_get_value_int(&config, "PARAMS", "buffer_size", &buffer_size);
	if(!buffer_size)
	{
		buffer_size = 2048;
		printf("\n Buffer size not specified, setting to %d ",buffer_size);
	}

	configuration_get_value_int(&config, "PARAMS", "chunk_size", &chunk_size);
	if(!chunk_size)
	{
	       chunk_size = 32;
	       printf("\n Chunk size not specified, setting to %d ", chunk_size);
	}
	configuration_get_value_int(&config, "PARAMS", "num_vc", &num_vc);
	if(!num_vc)
	{
		num_vc = 1; /*by default, we have one for taking packets, another for taking credit*/
		printf("\n num_vc not specified, setting to %d ", num_vc);
	}

        configuration_get_value(&config, "PARAMS", "dim_length", dim_length, MAX_NAME_LENGTH);
        char* token;
	net_params.n_dims=n_dims;
	net_params.num_vc=num_vc;
	net_params.buffer_size=buffer_size;
	net_params.chunk_size = chunk_size;
	net_params.link_bandwidth=link_bandwidth;
	net_params.dim_length=malloc(n_dims*sizeof(int));
        token = strtok(dim_length, ",");	
	while(token != NULL)
	{
	   sscanf(token, "%d", &net_params.dim_length[i]);
	   if(!net_params.dim_length[i])
	   {
	      printf("\n Invalid torus dimension specified %d, exitting... ", net_params.dim_length[i]);
	      MPI_Finalize();
	      exit(-1);
	   }
	   i++;
	   token = strtok(NULL,",");
	}
	net_id = model_net_setup(model_net_method_names[TORUS], packet_size, (const void*)&net_params);
     }
  else
       printf("\n Invalid network argument %s ", mn_name);
  model_net_base_init();
  return net_id;
}

void model_net_event_rc(
        int net_id,
        tw_lp *sender,
        uint64_t message_size){
    model_net_event_impl_base_rc(sender);
}

void model_net_pull_event_rc(
        int net_id,
        tw_lp *sender) {
    model_net_event_impl_base_rc(sender);
}

/* returns the message size, can be either simplenet, dragonfly or torus message size*/
int model_net_get_msg_sz(int net_id)
{
   // TODO: Add checks on network name
   // TODO: Add dragonfly and torus network models
   return sizeof(model_net_wrap_msg);
#if 0
   if(net_id < 0 || net_id >= MAX_NETS)
     {
      printf("%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
      exit(-1);
     }

       return method_array[net_id]->mn_get_msg_sz();
#endif
}

/* returns the packet size in the modelnet struct */
uint64_t model_net_get_packet_size(int net_id)
{
  if(net_id < 0 || net_id >= MAX_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }
  return method_array[net_id]->packet_size; // TODO: where to set the packet size?
}

/* This event does a collective operation call for model-net */
void model_net_event_collective(int net_id, char* category, int message_size, int remote_event_size, const void* remote_event, tw_lp* sender)
{
  if(net_id < 0 || net_id > MAX_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }
  return method_array[net_id]->mn_collective_call(category, message_size, remote_event_size, remote_event, sender);
}

/* reverse event of the collective operation call */
void model_net_event_collective_rc(int net_id, int message_size, tw_lp* sender)
{
  if(net_id < 0 || net_id > MAX_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }
  return method_array[net_id]->mn_collective_call_rc(message_size, sender);
}

/* returns lp type for modelnet */
const tw_lptype* model_net_get_lp_type(int net_id)
{
    if(net_id < 0 || net_id >= MAX_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }

   // TODO: ADd checks by network names
   // Add dragonfly and torus network models
   return method_array[net_id]->mn_get_lp_type();
}

void model_net_report_stats(int net_id)
{
  if(net_id < 0 || net_id >= MAX_NETS)
  {
    fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
    exit(-1);
   }

     // TODO: ADd checks by network names
     //    // Add dragonfly and torus network models
   method_array[net_id]->mn_report_stats();
   return;
}

tw_lpid model_net_find_local_device(int net_id, tw_lp *sender)
{
    return(method_array[net_id]->model_net_method_find_local_device(sender));
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
