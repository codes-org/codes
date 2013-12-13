/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "codes/dragonfly.h"

// Local router ID: 0 --- total_router-1
// Router LP ID 
// Terminal LP ID

/* setup the torus model, initialize global parameters */
static void dragonfly_setup(const void* net_params)
{
   dragonfly_param* d_param = (dragonfly_param*)net_params;

   num_vcs = d_param->num_vcs;
   num_routers = d_param->num_routers;
   num_cn = num_routers/2;
   num_global_channels = num_routers/2;
   num_groups = num_routers * num_cn + 1; 

   global_bandwidth = d_param->global_bandwidth;
   local_bandwidth = d_param->local_bandwidth;
   cn_bandwidth = d_param->cn_bandwidth;

   global_vc_size = d_param->global_vc_size;
   local_vc_size = d_param->local_vc_size;
   cn_vc_size = d_param->cn_vc_size;
   routing = d_param->routing;

   radix = num_vcs * (num_cn + num_global_channels + num_routers);
   total_routers = num_groups * num_routers;
   lp_type_register("dragonfly_router", dragonfly_get_router_lp_type());
   return;
}

/* report dragonfly statistics like average and maximum packet latency, average number of hops traversed */
static void dragonfly_report_stats()
{
/* TODO: Add dragonfly packet average, maximum latency and average number of hops traversed */
   long long avg_hops, total_finished_packets;
   tw_stime avg_time, max_time;

   MPI_Reduce( &total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &dragonfly_total_time, &avg_time, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &dragonfly_max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

   /* print statistics */
   if(!g_tw_mynode)
   {
      printf(" Average number of hops traversed %f average message latency %lf us maximum message latency %lf us \n", (float)avg_hops/total_finished_packets, avg_time/(total_finished_packets*1000), max_time/1000);
   }
   return;
}
/* dragonfly packet event , generates a dragonfly packet on the compute node */
static void dragonfly_packet_event(char* category, tw_lpid final_dest_lp, int packet_size, int remote_event_size, const void* remote_event, int self_event_size, const void* self_event, tw_lp *sender, int is_last_pckt)
{
    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    terminal_message * msg;
    tw_lpid local_nic_id, dest_nic_id;
    char* tmp_ptr;
    char lp_type_name[MAX_NAME_LENGTH], lp_group_name[MAX_NAME_LENGTH];

    int mapping_grp_id, mapping_rep_id, mapping_type_id, mapping_offset;
    codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(lp_group_name, "modelnet_dragonfly", mapping_rep_id, mapping_offset, &local_nic_id);

    codes_mapping_get_lp_info(final_dest_lp, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(lp_group_name, "modelnet_dragonfly", mapping_rep_id, mapping_offset, &dest_nic_id);
  
    xfer_to_nic_time = 0.01 + codes_local_latency(sender); /* Throws an error of found last KP time > current event time otherwise when LPs of one type are placed together*/
    e_new = codes_event_new(local_nic_id, xfer_to_nic_time, sender);
    msg = tw_event_data(e_new);
    strcpy(msg->category, category);
    msg->final_dest_gid = final_dest_lp;
    msg->dest_terminal_id = dest_nic_id;
    msg->sender_lp=sender->gid;
    msg->packet_size = packet_size;
    msg->remote_event_size_bytes = 0;
    msg->local_event_size_bytes = 0;
    msg->type = T_GENERATE;

    if(is_last_pckt) /* Its the last packet so pass in remote and local event information*/
      {
	tmp_ptr = (char*)msg;
	tmp_ptr += dragonfly_get_msg_sz();
	if(remote_event_size > 0)
	 {
		msg->remote_event_size_bytes = remote_event_size;
		memcpy(tmp_ptr, remote_event, remote_event_size);
		tmp_ptr += remote_event_size;
	}
	if(self_event_size > 0)
	{
		msg->local_event_size_bytes = self_event_size;
		memcpy(tmp_ptr, self_event, self_event_size);
		tmp_ptr += self_event_size;
	}
     }
	   //printf("\n torus remote event %d local event %d last packet %d %lf ", msg->remote_event_size_bytes, msg->local_event_size_bytes, is_last_pckt, xfer_to_nic_time);
    tw_event_send(e_new);
    return;
}

/* returns the torus message size */
static int dragonfly_get_msg_sz(void)
{
	   return sizeof(terminal_message);
}

/* dragonfly packet event reverse handler */
static void dragonfly_packet_event_rc(tw_lp *sender)
{
	  codes_local_latency_reverse(sender);
	    return;
}

/* given a group ID gid, find the router in the current group that is attached
 * to a router in the group gid */
tw_lpid getRouterFromGroupID(int gid, 
		    router_state * r)
{
  int group_begin = r->group_id * num_routers;
  int group_end = (r->group_id * num_routers) + num_routers-1;
  int offset = (gid * num_routers - group_begin) / num_routers;
  
  if((gid * num_routers) < group_begin)
    offset = (group_begin - gid * num_routers) / num_routers; // take absolute value
  
  int half_channel = num_global_channels / 2;
  int index = (offset - 1)/(half_channel * num_routers);
  
  offset=(offset - 1) % (half_channel * num_routers);

  // If the destination router is in the same group
  tw_lpid router_id;

  if(index % 2 != 0)
    router_id = group_end - (offset / half_channel); // start from the end
  else
    router_id = group_begin + (offset / half_channel);

  return router_id;
}	

/*When a packet is sent from the current router and a buffer slot becomes available, a credit is sent back to schedule another packet event*/
void router_credit_send(router_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
  tw_event * buf_e;
  tw_stime ts;
  terminal_message * buf_msg;

  int dest=0, credit_delay=0, type = R_BUFFER;

 // Notify sender terminal about available buffer space
  if(msg->last_hop == TERMINAL)
  {
   dest = msg->src_terminal_id;
   //determine the time in ns to transfer the credit
   credit_delay = (1 / cn_bandwidth) * CREDIT_SIZE;
   type = T_BUFFER;
  }
   else if(msg->last_hop == GLOBAL)
   {
     dest = msg->intm_lp_id;
     credit_delay = (1 / global_bandwidth) * CREDIT_SIZE;
   }
    else if(msg->last_hop == LOCAL)
     {
        dest = msg->intm_lp_id;
     	credit_delay = (1/local_bandwidth) * CREDIT_SIZE;
     }
    else
      printf("\n Invalid message type");

   // Assume it takes 0.1 ns of serialization latency for processing the credits in the queue
    int output_port = msg->saved_vc / num_vcs;
    msg->saved_available_time = s->next_credit_available_time[output_port];
    s->next_credit_available_time[output_port] = max(tw_now(lp), s->next_credit_available_time[output_port]);
    ts = credit_delay + tw_rand_exponential(lp->rng, (double)credit_delay/1000);
	
    s->next_credit_available_time[output_port]+=ts;
    buf_e = tw_event_new(dest, s->next_credit_available_time[output_port] - tw_now(lp) , lp);
    buf_msg = tw_event_data(buf_e);
    buf_msg->vc_index = msg->saved_vc;
    buf_msg->type=type;
    buf_msg->last_hop = msg->last_hop;
    buf_msg->packet_ID=msg->packet_ID;

    tw_event_send(buf_e);

    return;
}

/* generates packet at the current dragonfly compute node */
void packet_generate(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
  tw_stime ts;
  tw_event *e;
  terminal_message *m;
  int i, total_event_size;
  num_chunks = msg->packet_size / CHUNK_SIZE;
  msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter + tw_rand_integer(lp->rng, 0, lp->gid + g_tw_nlp * s->packet_counter);
  msg->travel_start_time = tw_now(lp);
  msg->my_N_hop = 0;
  for(i = 0; i < num_chunks; i++)
  {
	  // Before
	  // msg->my_N_hop = 0; generating a packet, check if the input queue is available
        ts = i + tw_rand_exponential(lp->rng, MEAN_INTERVAL/200);
	int chan = -1, j;
	for(j = 0; j < num_vcs; j++)
	 {
	     if(s->vc_occupancy[j] < cn_vc_size * num_chunks)
	      {
	       chan=j;
	       break;
	      }
         }

       e = tw_event_new(lp->gid, i + ts, lp);
       m = tw_event_data(e);
       memcpy(m, msg, dragonfly_get_msg_sz() + msg->remote_event_size_bytes + msg->local_event_size_bytes);
       m->intm_group_id = -1;
       m->saved_vc=0;
       m->chunk_id = i;
       
       if(msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
         printf("\n packet generated %lld at terminal %d chunk id %d ", msg->packet_ID, (int)lp->gid, i);
       
       m->output_chan = -1;
       if(chan != -1) // If the input queue is available
   	{
	    // Send the packet out
	     m->type = T_SEND;
 	     tw_event_send(e);
        }
      else
         {
	  printf("\n Exceeded queue size, exitting %d", s->vc_occupancy[0]);
	  MPI_Finalize();
	  exit(-1);
        } //else
  } // for
  total_event_size = dragonfly_get_msg_sz() + msg->remote_event_size_bytes + msg->local_event_size_bytes;
  mn_stats* stat;
  stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
  stat->send_count++;
  stat->send_bytes += msg->packet_size;
  stat->send_time += (1/cn_bandwidth) * msg->packet_size;
  if(stat->max_event_size < total_event_size)
	  stat->max_event_size = total_event_size;
  return;
}

/* sends the packet from the current dragonfly compute node to the attached router */
void packet_send(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
  tw_stime ts;
  tw_event *e;
  terminal_message *m;
  tw_lpid router_id;
  /* Route the packet to its source router */ 
   int vc=msg->saved_vc;

   //  Each packet is broken into chunks and then sent over the channel
   msg->saved_available_time = s->terminal_available_time;
   head_delay = (1/cn_bandwidth) * CHUNK_SIZE;
   ts = head_delay + tw_rand_exponential(lp->rng, (double)head_delay/200);
   s->terminal_available_time = max(s->terminal_available_time, tw_now(lp));
   s->terminal_available_time += ts;

   codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
   codes_mapping_get_lp_id(lp_group_name, "dragonfly_router", s->router_id, 0, &router_id);
   e = tw_event_new(router_id, s->terminal_available_time - tw_now(lp), lp);

   if(msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
     printf("\n terminal %d packet %lld chunk %d being sent to router %d router id %d ", (int)lp->gid, (long long)msg->packet_ID, msg->chunk_id, (int)router_id, s->router_id);
   m = tw_event_data(e);
   memcpy(m, msg, dragonfly_get_msg_sz() + msg->remote_event_size_bytes);
   m->type = R_ARRIVE;
   m->src_terminal_id = lp->gid;
   m->saved_vc = vc;
   m->last_hop = TERMINAL;
   m->intm_group_id = -1;
   m->local_event_size_bytes = 0;
   tw_event_send(e);
//  Each chunk is 32B and the VC occupancy is in chunks to enable efficient flow control

   if(msg->chunk_id == num_chunks - 1) 
    {
      /* local completion message */
      if(msg->local_event_size_bytes > 0)
	 {
           tw_event* e_new;
	   terminal_message* m_new;
	   char* local_event;
	   ts = (1/cn_bandwidth) * msg->local_event_size_bytes;
	   e_new = codes_event_new(msg->sender_lp, ts, lp);
	   m_new = tw_event_data(e_new);
	   local_event = (char*)msg;
	   local_event += dragonfly_get_msg_sz() + msg->remote_event_size_bytes;
	   memcpy(m_new, local_event, msg->local_event_size_bytes);
	   tw_event_send(e_new);
	}
    }
   
   s->packet_counter++;
   s->vc_occupancy[vc]++;

   if(s->vc_occupancy[vc] >= (cn_vc_size * num_chunks))
      s->output_vc_state[vc] = VC_CREDIT;
   return;
}

/* packet arrives at the destination terminal */
void packet_arrive(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
#if DEBUG
if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
    {
	printf( "(%lf) [Terminal %d] packet %lld has arrived  \n",
              tw_now(lp), (int)lp->gid, msg->packet_ID);

	printf("travel start time is %f\n",
                msg->travel_start_time);

	printf("My hop now is %d\n",msg->my_N_hop);
    }
#endif

  // Packet arrives and accumulate # queued
  // Find a queue with an empty buffer slot
   tw_event * e, * buf_e;
   terminal_message * m, * buf_msg;
   tw_stime ts;
   bf->c3 = 0;
   bf->c2 = 0;

   msg->my_N_hop++;
  if(msg->chunk_id == num_chunks-1)
  {
	 bf->c2 = 1;
	 mn_stats* stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
	 stat->recv_count++;
	 stat->recv_bytes += msg->packet_size;
	 stat->recv_time += tw_now(lp) - msg->travel_start_time;

	 N_finished_packets++;
	 dragonfly_total_time += tw_now( lp ) - msg->travel_start_time;
	 total_hops += msg->my_N_hop;

	 if (dragonfly_max_latency < tw_now( lp ) - msg->travel_start_time) 
	 {
		bf->c3 = 1;
		msg->saved_available_time = dragonfly_max_latency;
		dragonfly_max_latency=tw_now( lp ) - msg->travel_start_time;
	 }
	// Trigger an event on receiving server
	if(msg->remote_event_size_bytes)
	{
		ts = (1/cn_bandwidth) * msg->remote_event_size_bytes;
		e = codes_event_new(msg->final_dest_gid, ts, lp);
		m = tw_event_data(e);
		char* tmp_ptr = (char*)msg;
		tmp_ptr += dragonfly_get_msg_sz();                                                                                                            
		memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
		tw_event_send(e); 
	}
  }

  int credit_delay = (1 / cn_bandwidth) * CREDIT_SIZE;
  ts = credit_delay + tw_rand_exponential(lp->rng, credit_delay/1000);
  
  msg->saved_credit_time = s->next_credit_available_time;
  s->next_credit_available_time = max(s->next_credit_available_time, tw_now(lp));
  s->next_credit_available_time += ts;

  tw_lpid router_dest_id;
  codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
  codes_mapping_get_lp_id(lp_group_name, "dragonfly_router", s->router_id, 0, &router_dest_id);
  buf_e = tw_event_new(router_dest_id, s->next_credit_available_time - tw_now(lp), lp);
  buf_msg = tw_event_data(buf_e);
  buf_msg->vc_index = msg->saved_vc;
  buf_msg->type=R_BUFFER;
  buf_msg->packet_ID=msg->packet_ID;
  buf_msg->last_hop = TERMINAL;
  tw_event_send(buf_e);

  return;
}

/* initialize a dragonfly compute node terminal */
void 
terminal_init( terminal_state * s, 
	       tw_lp * lp )
{
    int i;
    // Assign the global router ID
   codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
   int num_lps = codes_mapping_get_lp_count(lp_group_name, "modelnet_dragonfly");

   s->terminal_id = (mapping_rep_id * num_lps) + mapping_offset;  
   s->router_id=(int)s->terminal_id / num_routers;
   s->terminal_available_time = 0.0;
   s->packet_counter = 0;

   s->vc_occupancy = (int*)malloc(num_vcs * sizeof(int));
   s->output_vc_state = (int*)malloc(num_vcs * sizeof(int));

   for( i = 0; i < num_vcs; i++ )
    {
      s->vc_occupancy[i]=0;
      s->output_vc_state[i]=VC_IDLE;
    }
   return;
}

/* update the compute node-router channel buffer */
void 
terminal_buf_update(terminal_state * s, 
		    tw_bf * bf, 
		    terminal_message * msg, 
		    tw_lp * lp)
{
  // Update the buffer space associated with this router LP 
    int msg_indx = msg->vc_index;
    
    s->vc_occupancy[msg_indx]--;
    s->output_vc_state[msg_indx] = VC_IDLE;

    return;
}

void 
terminal_event( terminal_state * s, 
		tw_bf * bf, 
		terminal_message * msg, 
		tw_lp * lp )
{
  *(int *)bf = (int)0;
  switch(msg->type)
    {
    case T_GENERATE:
       packet_generate(s,bf,msg,lp);
    break;
    
    case T_ARRIVE:
        packet_arrive(s,bf,msg,lp);
    break;
    
    case T_SEND:
      packet_send(s,bf,msg,lp);
    break;
    
    case T_BUFFER:
       terminal_buf_update(s, bf, msg, lp);
     break;

    default:
       printf("\n LP %d Terminal message type not supported %d ", (int)lp->gid, msg->type);
    }
}

void 
dragonfly_terminal_final( terminal_state * s, 
      tw_lp * lp )
{
	model_net_print_stats(lp->gid, s->dragonfly_stats_array);
}

void dragonfly_router_final(router_state * s,
		tw_lp * lp)
{
   free(s->global_channel);
}
/* get the next stop for the current packet
 * determines if it is a router within a group, a router in another group
 * or the destination terminal */
tw_lpid 
get_next_stop(router_state * s, 
		      tw_bf * bf, 
		      terminal_message * msg, 
		      tw_lp * lp, 
		      int path)
{
   int dest_lp;
   tw_lpid router_dest_id = -1;
   int i;
   int dest_group_id;

   codes_mapping_get_lp_info(msg->dest_terminal_id, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset); 
   int num_lps = codes_mapping_get_lp_count(lp_group_name, "modelnet_dragonfly");
   int dest_router_id = (mapping_offset + (mapping_rep_id * num_lps)) / num_routers;
   
   codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
   int local_router_id = (mapping_offset + mapping_rep_id);

   bf->c2 = 0;

   if(dest_router_id == local_router_id)
    {
        dest_lp = msg->dest_terminal_id;

        return dest_lp;
    }
   // Generate inter-mediate destination
   if(msg->last_hop == TERMINAL && path == NON_MINIMAL)
    {
      if(dest_router_id / num_routers != s->group_id)
         {
            bf->c2 = 1;
            int intm_grp_id = tw_rand_integer(lp->rng, 0, num_groups-1);
            msg->intm_group_id = intm_grp_id;
          }    
    }
   if(msg->intm_group_id == s->group_id)
   {  
           msg->intm_group_id = -1;//no inter-mediate group
   } 
  if(msg->intm_group_id >= 0)
   {
      dest_group_id = msg->intm_group_id;
   }
  else
   {
     dest_group_id = dest_router_id / num_routers;
   }
  
  if(s->group_id == dest_group_id)
   {
     dest_lp = dest_router_id;
   }
   else
   {
      dest_lp=getRouterFromGroupID(dest_group_id,s);
  
      if(dest_lp == local_router_id)
      {
        for(i=0; i < num_global_channels; i++)
           {
            if(s->global_channel[i] / num_routers == dest_group_id)
                dest_lp=s->global_channel[i];
          }
      }
   }
  codes_mapping_get_lp_id(lp_group_name, "dragonfly_router", dest_lp, 0, &router_dest_id);
  return router_dest_id;
}

/* gets the output port corresponding to the next stop of the message */
int 
get_output_port( router_state * s, 
		tw_bf * bf, 
		terminal_message * msg, 
		tw_lp * lp, 
		int next_stop )
{
  int output_port = -1, i, terminal_id;
  codes_mapping_get_lp_info(msg->dest_terminal_id, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
  int num_lps = codes_mapping_get_lp_count(lp_group_name,"modelnet_dragonfly");
  terminal_id = (mapping_rep_id * num_lps) + mapping_offset;

  if(next_stop == msg->dest_terminal_id)
   {
      output_port = num_routers + num_global_channels + ( terminal_id % num_cn);
      //if(output_port > 6)
	//      printf("\n incorrect output port %d terminal id %d ", output_port, terminal_id);
    }
    else
    {
     codes_mapping_get_lp_info(next_stop, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
     int local_router_id = mapping_rep_id + mapping_offset;
     int intm_grp_id = local_router_id / num_routers;

     if(intm_grp_id != s->group_id)
      {
        for(i=0; i < num_global_channels; i++)
         {
           if(s->global_channel[i] == local_router_id)
             output_port = num_routers + i;
          }
      }
      else
       {
        output_port = local_router_id % num_routers;
       }
      if(output_port == 6)
	      printf("\n output port not found %d next stop %d local router id %d group id %d intm grp id %d %d", output_port, next_stop, local_router_id, s->group_id, intm_grp_id, local_router_id%num_routers);
    }
    return output_port;
}

/* routes the current packet to the next stop */
void 
router_packet_send( router_state * s, 
		    tw_bf * bf, 
		     terminal_message * msg, tw_lp * lp)
{
   tw_stime ts;
   tw_event *e;
   terminal_message *m;

   int next_stop = -1, output_port = -1, output_chan = -1;
   float bandwidth = local_bandwidth;
   int path = routing;

   bf->c3 = 0;

   next_stop = get_next_stop(s, bf, msg, lp, path);
   output_port = get_output_port(s, bf, msg, lp, next_stop); 
   output_chan = output_port * num_vcs;

   // Even numbered channels for minimal routing
   // Odd numbered channels for nonminimal routing
   int global=0;
   int buf_size = local_vc_size;

   assert(output_port != -1);
   assert(output_chan != -1);
   // Allocate output Virtual Channel
  if(output_port >= num_routers && output_port < num_routers + num_global_channels)
  {
	 bandwidth = global_bandwidth;
	 global = 1;
	 buf_size = global_vc_size;
  }

  if(output_port >= num_routers + num_global_channels)
	buf_size = cn_vc_size;

   if(s->vc_occupancy[output_chan] >= buf_size)
    {
	    printf("\n %lf Router %ld buffers overflowed from incoming terminals channel %d occupancy %d radix %d next_stop %d ", tw_now(lp),(long int) lp->gid, output_chan, s->vc_occupancy[output_chan], radix, next_stop);
	    bf->c3 = 1;
	    MPI_Finalize();
	    exit(-1);
    }

#if DEBUG
if( msg->packet_ID == TRACK && next_stop != msg->dest_terminal_id && msg->chunk_id == num_chunks-1)
  {
   printf("\n (%lf) [Router %d] Packet %lld being sent to intermediate group router %d Final destination terminal %d Output Channel Index %d Saved vc %d msg_intm_id %d \n", 
              tw_now(lp), (int)lp->gid, msg->packet_ID, next_stop, 
	      msg->dest_terminal_id, output_chan, msg->saved_vc, msg->intm_group_id);
  }
#endif
 // If source router doesn't have global channel and buffer space is available, then assign to appropriate intra-group virtual channel 
  msg->saved_available_time = s->next_output_available_time[output_port];
  ts = ((1/bandwidth) * CHUNK_SIZE) + tw_rand_exponential(lp->rng, (double)CHUNK_SIZE/200);

  s->next_output_available_time[output_port] = max(s->next_output_available_time[output_port], tw_now(lp));
  s->next_output_available_time[output_port] += ts;
  e = tw_event_new(next_stop, s->next_output_available_time[output_port] - tw_now(lp), lp);

  m = tw_event_data(e);
  memcpy(m, msg, dragonfly_get_msg_sz() + msg->remote_event_size_bytes);

  if(global)
    m->last_hop=GLOBAL;
  else
    m->last_hop = LOCAL;

  m->saved_vc = output_chan;
  msg->old_vc = output_chan;
  m->intm_lp_id = lp->gid;
  s->vc_occupancy[output_chan]++;

  if(next_stop == msg->dest_terminal_id)
  {
    m->type = T_ARRIVE;

    if(s->vc_occupancy[output_chan] >= cn_vc_size * num_chunks)
      s->output_vc_state[output_chan] = VC_CREDIT;
  }
  else
  {
    m->type = R_ARRIVE;

   if( global )
   {
     if(s->vc_occupancy[output_chan] >= global_vc_size * num_chunks )
       s->output_vc_state[output_chan] = VC_CREDIT;
   }
  else
    {
     if( s->vc_occupancy[output_chan] >= local_vc_size * num_chunks )
	s->output_vc_state[output_chan] = VC_CREDIT;
    }
  }
  tw_event_send(e);
  return;
}

/* Packet arrives at the router and a credit is sent back to the sending terminal/router */
void 
router_packet_receive( router_state * s, 
			tw_bf * bf, 
			terminal_message * msg, 
			tw_lp * lp )
{
    tw_event *e;
    terminal_message *m;
    tw_stime ts;

    msg->my_N_hop++;
    ts = 0.1 + tw_rand_exponential(lp->rng, (double)MEAN_INTERVAL/200);
    num_chunks = msg->packet_size/CHUNK_SIZE;

    if(msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
       printf("\n packet %lld chunk %d received at router %d ", msg->packet_ID, msg->chunk_id, (int)lp->gid);
   
    e = tw_event_new(lp->gid, ts, lp);
    m = tw_event_data(e);
    memcpy(m, msg, dragonfly_get_msg_sz() + msg->remote_event_size_bytes);
    m->type = R_SEND;
    router_credit_send(s, bf, msg, lp);
    tw_event_send(e);  
    return;
}

/* sets up the router virtual channels, global channels, local channels, compute node channels */
void router_setup(router_state * r, tw_lp * lp)
{
   codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
   r->router_id=mapping_rep_id + mapping_offset;
   r->group_id=r->router_id/num_routers;

   int i;
   int router_offset=(r->router_id % num_routers) * (num_global_channels / 2) + 1;

   r->global_channel = (int*)malloc(num_global_channels * sizeof(int));
   r->next_output_available_time = (tw_stime*)malloc(radix * sizeof(tw_stime));
   r->next_credit_available_time = (tw_stime*)malloc(radix * sizeof(tw_stime));
   r->vc_occupancy = (int*)malloc(radix * sizeof(int));
   r->output_vc_state = (int*)malloc(radix * sizeof(int));
  
   for(i=0; i < radix; i++)
    {
       // Set credit & router occupancy
	r->next_output_available_time[i]=0;
        r->next_credit_available_time[i]=0;
        r->vc_occupancy[i]=0;
        r->output_vc_state[i]= VC_IDLE;
    }

   //round the number of global channels to the nearest even number
   for(i=0; i < num_global_channels; i++)
    {
      if(i % 2 != 0)
          {
             r->global_channel[i]=(r->router_id + (router_offset * num_routers))%total_routers;
             router_offset++;
          }
          else
           {
             r->global_channel[i]=r->router_id - ((router_offset) * num_routers);
           }
        if(r->global_channel[i]<0)
         {
           r->global_channel[i]=total_routers+r->global_channel[i]; 
	 }
    }
   return;
}	

/* Update the buffer space associated with this router LP */
void router_buf_update(router_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
    int msg_indx = msg->vc_index;
    s->vc_occupancy[msg_indx]--;
    s->output_vc_state[msg_indx] = VC_IDLE;
    return;
}

void router_event(router_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
  *(int *)bf = (int)0;
  switch(msg->type)
   {
	   case R_SEND: // Router has sent a packet to an intra-group router (local channel)
 		 router_packet_send(s, bf, msg, lp);
           break;

	   case R_ARRIVE: // Router has received a packet from an intra-group router (local channel)
	        router_packet_receive(s, bf, msg, lp);
	   break;
	
	   case R_BUFFER:
	        router_buf_update(s, bf, msg, lp);
	   break;

	   default:
		  printf("\n (%lf) [Router %d] Router Message type not supported %d dest terminal id %d packet ID %d ", tw_now(lp), (int)lp->gid, msg->type, (int)msg->dest_terminal_id, (int)msg->packet_ID);
	   break;
   }	   
}

/* Reverse computation handler for a terminal event */
void terminal_rc_event_handler(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
   switch(msg->type)
   {
	   case T_GENERATE:
		 {
		 int i;
		 tw_rand_reverse_unif(lp->rng);

		 for(i = 0; i < num_chunks; i++)
                  tw_rand_reverse_unif(lp->rng);
		 mn_stats* stat;
		 stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
		 stat->send_count--;
		 stat->send_bytes -= msg->packet_size;
		 stat->send_time -= (1/cn_bandwidth) * msg->packet_size;
		 }
	   break;
	   
	   case T_SEND:
	         {
	           s->terminal_available_time = msg->saved_available_time;
		   tw_rand_reverse_unif(lp->rng);	
		   int vc = msg->saved_vc;
		   s->vc_occupancy[vc]--;
		   s->packet_counter--;
		   s->output_vc_state[vc] = VC_IDLE;
		 }
	   break;

	   case T_ARRIVE:
	   	 {
		   tw_rand_reverse_unif(lp->rng);
		   s->next_credit_available_time = msg->saved_credit_time;
		   if(bf->c2)
		   {
		    mn_stats* stat;
		    stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
		    stat->recv_count--;
		    stat->recv_bytes -= msg->packet_size;
		    stat->recv_time -= tw_now(lp) - msg->travel_start_time;
		    N_finished_packets--;
		    dragonfly_total_time -= (tw_now(lp) - msg->travel_start_time);
		    total_hops -= msg->my_N_hop;
		   if(bf->c3)
		         dragonfly_max_latency = msg->saved_available_time;
		   }
		    
		   msg->my_N_hop--;
		 }
           break;

	   case T_BUFFER:
	        {
		   int msg_indx = msg->vc_index;
		   s->vc_occupancy[msg_indx]++;
		   
		   if(s->vc_occupancy[msg_indx] == cn_vc_size * num_chunks)
			s->output_vc_state[msg_indx] = VC_CREDIT;
	     }  
	   break;
   }
}

/* Reverse computation handler for a router event */
void router_rc_event_handler(router_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
  switch(msg->type)
    {
            case R_SEND:
		    {
		        tw_rand_reverse_unif(lp->rng);

			if(bf->c3)
			   return;
			    
			int output_chan = msg->old_vc;
			int output_port = output_chan / num_vcs;

			s->next_output_available_time[output_port] = msg->saved_available_time;
			s->vc_occupancy[output_chan]--;
			s->output_vc_state[output_chan]=VC_IDLE;
		
			if(bf->c2)
			   tw_rand_reverse_unif(lp->rng);
		    }
	    break;

	    case R_ARRIVE:
	    	    {
			msg->my_N_hop--;
			tw_rand_reverse_unif(lp->rng);
			tw_rand_reverse_unif(lp->rng);
			int output_port = msg->saved_vc/num_vcs;
			s->next_credit_available_time[output_port] = msg->saved_available_time;
		    }
	    break;

	    case R_BUFFER:
	    	   {
		      int msg_indx = msg->vc_index;
                      s->vc_occupancy[msg_indx]++;

                      int buf = local_vc_size;

		      if(msg->last_hop == GLOBAL)
			 buf = global_vc_size;
		       else if(msg->last_hop == TERMINAL)
			 buf = cn_vc_size;
	 
		      if(s->vc_occupancy[msg_indx] >= buf * num_chunks)
                          s->output_vc_state[msg_indx] = VC_CREDIT;

		   }
	    break;
	  
    }
}
/* dragonfly compute node and router LP types */
tw_lptype dragonfly_lps[] =
{
   // Terminal handling functions
   {
    (init_f)terminal_init,
    (event_f) terminal_event,
    (revent_f) terminal_rc_event_handler,
    (final_f) dragonfly_terminal_final,
    (map_f) codes_mapping,
    sizeof(terminal_state)
    },
   {
     (init_f) router_setup,
     (event_f) router_event,
     (revent_f) router_rc_event_handler,
     (final_f) dragonfly_router_final,
     (map_f) codes_mapping,
     sizeof(router_state),
   },
   {0},
};

/* returns the dragonfly lp type for lp registration */
static const tw_lptype* dragonfly_get_cn_lp_type(void)
{
	   return(&dragonfly_lps[0]);
}

/* returns the dragonfly router lp type for lp registration */
static const tw_lptype* dragonfly_get_router_lp_type(void)
{
	           return(&dragonfly_lps[1]);
}          



/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
