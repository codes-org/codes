/*
 * Copyright (C) 2013, University of Chicago
 *
 * See COPYRIGHT notice in top-level directory.
 */
#include <string.h>
#include <assert.h>

#include "model-net.h"
#include "model-net-method.h"

#define STR_SIZE 16
#define PROC_TIME 10.0
#define NUM_NETS 1

extern struct model_net_method simplenet_method;
//extern struct dragonfly_method dragonfly_method;
//extern struct torus_method torus_method;

/* Global array initialization, terminated with a NULL entry */
static struct model_net_method* method_array[] =
    {&simplenet_method, NULL};

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
	   return(i);
	}
     }
     fprintf(stderr, "Error: undefined network name %s (Available options dragonfly, torus, simplenet) \n", name);
     return -1; // indicating error
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

     if(message_size % packet_size)
	num_packets++; /* Handle the left out data if message size is not exactly divisible by packet size */

     /*Determine the network name*/
     if(net_id < 0 || net_id > NUM_NETS)
     {
        fprintf(stderr, "Error: undefined network ID %d (Available options 0 (simplenet), 1 (dragonfly) 2 (torus) ) \n", net_id);
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
const int model_net_get_msg_sz(int net_id)
{
   // TODO: Add checks on network name
   // TODO: Add dragonfly and torus network models
   if(net_id < 0 || net_id > NUM_NETS)
     {
      printf("%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
      exit(-1);
     }

       return method_array[net_id]->mn_get_msg_sz();
}

/* returns the packet size in the modelnet struct */
const int model_net_get_packet_size(int net_id)
{
  if(net_id < 0 || net_id > NUM_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }
  return method_array[net_id]->packet_size; // TODO: where to set the packet size?
}

/* returns lp type for modelnet */
const tw_lptype* model_net_get_lp_type(int net_id)
{
    if(net_id < 0 || net_id > NUM_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }

   // TODO: ADd checks by network names
   // Add dragonfly and torus network models
   return method_array[net_id]->mn_get_lp_type();
}

/* registers the lp type */
void model_net_add_lp_type(int net_id)
{
 switch(net_id)
 {
   case SIMPLENET:
       lp_type_register("modelnet_simplenet", model_net_get_lp_type(net_id));
   break;

   DEFAULT:
    {
        printf("\n Invalid net_id specified ");
	exit(-1);
    }
 }
}
/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
