/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <string.h>
#include <assert.h>

#include "codes/model-net.h"
#include "codes/model-net-method.h"

#define STR_SIZE 16
#define PROC_TIME 10.0

extern struct model_net_method simplenet_method;
extern struct model_net_method torus_method;
extern struct model_net_method dragonfly_method;
extern struct model_net_method loggp_method;

/* Global array initialization, terminated with a NULL entry */
static struct model_net_method* method_array[] =
    {&simplenet_method, &torus_method, &dragonfly_method, &loggp_method, NULL};

static int model_net_get_msg_sz(int net_id);

int model_net_setup(char* name,
		    int packet_size,
		    const void* net_params)
{
     int i;
    /* find struct for underlying method (according to configuration file) */
     for(i=0; method_array[i] != NULL; i++)
     {
     	if(strcmp(method_array[i]->method_name, name) == 0)
	{
	   method_array[i]->mn_setup(net_params);
	   method_array[i]->packet_size = packet_size;
	   model_net_add_lp_type(i);
	   return(i);
	}
     }
     fprintf(stderr, "Error: undefined network name %s (Available options simplenet, torus, dragonfly) \n", name);
     return -1; // indicating error
}

void model_net_write_stats(tw_lpid lpid, struct mn_stats* stat)
{
    int ret;
    char id[32];
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

void model_net_event(
    int net_id,
    char* category, 
    tw_lpid final_dest_lp, 
    int message_size, 
    int remote_event_size,
    const void* remote_event,
    int self_event_size,
    const void* self_event,
    tw_lp *sender)
{
    /* determine packet size for underlying method */
     int packet_size = model_net_get_packet_size(net_id);
     int num_packets = message_size/packet_size; /* Number of packets to be issued by the API */
     int i;
     int last = 0;

     //printf("\n number of packets %d message size %d ", num_packets, message_size);
     if((remote_event_size + self_event_size + model_net_get_msg_sz(net_id))
        > g_tw_msg_sz)
     {
        fprintf(stderr, "Error: model_net trying to transmit an event of size %d but ROSS is configured for events of size %zd\n",
            (remote_event_size + self_event_size + model_net_get_msg_sz(net_id)),
            g_tw_msg_sz);
        abort();
     }

     if(message_size % packet_size)
	num_packets++; /* Handle the left out data if message size is not exactly divisible by packet size */

     if(message_size < packet_size)
         num_packets = 1;

     /*Determine the network name*/
     if(net_id < 0 || net_id >= MAX_NETS)
     {
        fprintf(stderr, "Error: undefined network ID %d (Available options 0 (simplenet), 1 (torus) 2 (dragonfly) ) \n", net_id);
	exit(-1);
     }

    /* issue N packets using method API */
    /* somehow mark the final packet as the one responsible for delivering
     * the self event and remote event 
     *
     * local event is delivered to caller of this function, remote event is
     * passed along through network hops and delivered to final_dest_lp
     */

     for( i = 0; i < num_packets; i++ )
       {
	  /*Mark the last packet to the net method API*/
	   if(i == num_packets - 1)
           {
	      last = 1;
              /* also calculate the last packet's size */
              packet_size = message_size - ((num_packets-1)*packet_size);
            }
	  /* Number of packets and packet ID is passed to the underlying network to mark the final packet for local event completion*/
	  method_array[net_id]->model_net_method_packet_event(category, final_dest_lp, packet_size, remote_event_size, remote_event, self_event_size, self_event, sender, last);	  
       }
    return;
}

int model_net_set_params()
{
  char mn_name[MAX_NAME_LENGTH];
  int packet_size = 0;
  int net_id=-1;

  config_lpgroups_t paramconf;
  configuration_get_lpgroups(&config, "PARAMS", &paramconf);
  configuration_get_value(&config, "PARAMS", "modelnet", mn_name, MAX_NAME_LENGTH);
  configuration_get_value_int(&config, "PARAMS", "packet_size", &packet_size);

  if(!packet_size)
  {
	packet_size = 512;
	printf("\n Warning, no packet size specified, setting packet size to %d ", packet_size);
  }
  if(strcmp("simplenet",mn_name)==0)
   {
     double net_startup_ns, net_bw_mbps;
     simplenet_param net_params;
     
     configuration_get_value_double(&config, "PARAMS", "net_startup_ns", &net_startup_ns);
     configuration_get_value_double(&config, "PARAMS", "net_bw_mbps", &net_bw_mbps);
     net_params.net_startup_ns = net_startup_ns;
     net_params.net_bw_mbps =  net_bw_mbps;
     net_id = model_net_setup("simplenet", packet_size, (const void*)&net_params); /* Sets the network as simplenet and packet size 512 */
   }
   else if(strcmp("loggp",mn_name)==0)
   {
     char net_config_file[256];
     loggp_param net_params;
     
     configuration_get_value(&config, "PARAMS", "net_config_file", net_config_file, 256);
     net_params.net_config_file = net_config_file;
     net_id = model_net_setup("loggp", packet_size, (const void*)&net_params); /* Sets the network as loggp and packet size 512 */
   }

  else if(strcmp("dragonfly", mn_name)==0)	  
    {
       dragonfly_param net_params;
       int num_routers=0, num_vcs=0, local_vc_size=0, global_vc_size=0, cn_vc_size=0;
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
       else
       {
       	   printf("\n No routing protocol specified, setting to minimal routing");
   	   net_params.routing = 0;	   
       }
    net_id = model_net_setup("dragonfly", packet_size, (const void*)&net_params);   
    }
   else if(strcmp("torus", mn_name)==0)
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
	net_id = model_net_setup("torus", packet_size, (const void*)&net_params);
     }
  else
       printf("\n Invalid network argument %s ", mn_name);
  return net_id;
}
void model_net_event_rc(
    int net_id,
    tw_lp *sender,
    int message_size)
{
    /* this will be used for reverse computation of anything calculated
     * within th model_net_event() function call itself (not reverse
     * handling for the underlying methods, which will have their own events
     * and reverse handlers
     */
    int packet_size = model_net_get_packet_size(net_id);
    int num_packets = message_size/packet_size; /* For rolling back */
    int i;

    if(message_size % packet_size)
      num_packets++;

     for( i = 0; i < num_packets; i++ )
       {
	  /* Number of packets and packet ID is passed to the underlying network to mark the final packet for local event completion*/
	  method_array[net_id]->model_net_method_packet_event_rc(sender);	  
       }
    return;
} 

/* returns the message size, can be either simplenet, dragonfly or torus message size*/
static int model_net_get_msg_sz(int net_id)
{
   // TODO: Add checks on network name
   // TODO: Add dragonfly and torus network models
   if(net_id < 0 || net_id >= MAX_NETS)
     {
      printf("%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
      exit(-1);
     }

       return method_array[net_id]->mn_get_msg_sz();
}

/* returns the packet size in the modelnet struct */
int model_net_get_packet_size(int net_id)
{
  if(net_id < 0 || net_id >= MAX_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }
  return method_array[net_id]->packet_size; // TODO: where to set the packet size?
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
/* registers the lp type */
void model_net_add_lp_type(int net_id)
{
 switch(net_id)
 {
   case SIMPLENET:
       lp_type_register("modelnet_simplenet", model_net_get_lp_type(net_id));
   break;

   case TORUS:
       lp_type_register("modelnet_torus", model_net_get_lp_type(net_id));
       break;
   case DRAGONFLY:
       lp_type_register("modelnet_dragonfly", model_net_get_lp_type(net_id));
       break;
   case LOGGP:
       lp_type_register("modelnet_loggp", model_net_get_lp_type(net_id));
       break;
   default:
    {
        printf("\n Invalid net_id specified ");
	exit(-1);
    }
 }
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
