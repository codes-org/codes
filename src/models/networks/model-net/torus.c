#include "codes/torus.h"

/* setup the torus model, initialize global parameters */
static void torus_setup(const void* net_params)
{
    int i;
    torus_param* t_param = (torus_param*)net_params;
    n_dims = t_param->n_dims;
    link_bandwidth = t_param->link_bandwidth;
    buffer_size = t_param->buffer_size;
    num_vc = t_param->num_vc;
    
    head_delay = (1 / link_bandwidth) * CHUNK_SIZE;
    credit_delay = (1 / link_bandwidth) * 8;
    dim_length = malloc(n_dims * sizeof(int));
    factor = malloc(n_dims * sizeof(int));
    half_length = malloc(n_dims * sizeof(int));
    for(i = 0; i < n_dims; i++)
       dim_length[i] = t_param->dim_length[i]; /* TODO, read comma separated values from files */
}

static void torus_packet_event_rc(tw_lp *sender)
{
  codes_local_latency_reverse(sender);
  return;
}

/* torus packet event , generates a torus packet on the compute node */
static void torus_packet_event(char* category, tw_lpid final_dest_lp, int packet_size, int remote_event_size, const void* remote_event, int self_event_size, const void* self_event, tw_lp *sender, int is_last_pckt)
{
    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    nodes_message * msg;
    tw_lpid local_nic_id, dest_nic_id;
    char* tmp_ptr;
    char lp_type_name[MAX_NAME_LENGTH], lp_group_name[MAX_NAME_LENGTH];
   
    int mapping_grp_id, mapping_rep_id, mapping_type_id, mapping_offset;
    codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id("MODELNET_GRP", "modelnet_torus", mapping_rep_id, mapping_offset, &local_nic_id);

    codes_mapping_get_lp_info(final_dest_lp, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id("MODELNET_GRP", "modelnet_torus", mapping_rep_id, mapping_offset, &dest_nic_id);

    /* TODO: Should send the packets in correct sequence. Currently the last packet is being sent first due to codes_local_latency offset. */
    xfer_to_nic_time = 0.01 + codes_local_latency(sender); /* Throws an error of found last KP time > current event time otherwise */
    e_new = codes_event_new(local_nic_id, xfer_to_nic_time, sender);
    msg = tw_event_data(e_new);
    strcpy(msg->category, category);
    msg->final_dest_gid = final_dest_lp;
    msg->dest_lp = dest_nic_id;
    msg->sender_lp=sender->gid;
    msg->packet_size = packet_size;
    msg->remote_event_size_bytes = 0;
    msg->local_event_size_bytes = 0;
    msg->type = GENERATE;

    if(is_last_pckt) /* Its the last packet so pass in remote event information*/
     {
        tmp_ptr = (char*)msg;
        tmp_ptr += torus_get_msg_sz();
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
}
/*Initialize the torus model, this initialization part is borrowed from Ning's torus model */
static void torus_init( nodes_state * s, 
	   tw_lp * lp )
{
    int i, j;
    int dim_N[ n_dims + 1 ];

    codes_mapping_get_lp_info(lp->gid, grp_name, &grp_id, &lp_type_id, type_name, &rep_id, &offset);
    dim_N[ 0 ]=rep_id + offset;

    s->neighbour_minus_lpID = (int*)malloc(n_dims * sizeof(int));
    s->neighbour_plus_lpID = (int*)malloc(n_dims * sizeof(int));
    s->dim_position = (int*)malloc(n_dims * sizeof(int));
    s->buffer = (int**)malloc(2*n_dims * sizeof(int*));
    s->next_link_available_time = (tw_stime**)malloc(2*n_dims * sizeof(tw_stime*));
    s->next_credit_available_time = (tw_stime**)malloc(2*n_dims * sizeof(tw_stime*));
    s->next_flit_generate_time = (tw_stime**)malloc(2*n_dims*sizeof(tw_stime*));

    for(i=0; i < 2*n_dims; i++)
    {
	s->buffer[i] = (int*)malloc(num_vc * sizeof(int));
	s->next_link_available_time[i] = (tw_stime*)malloc(num_vc * sizeof(tw_stime));
	s->next_credit_available_time[i] = (tw_stime*)malloc(num_vc * sizeof(tw_stime));
	s->next_flit_generate_time[i] = (tw_stime*)malloc(num_vc * sizeof(tw_stime));
    }

    //printf("\n LP ID %d ", (int)lp->gid);
  // calculate my torus co-ordinates
  for ( i=0; i < n_dims; i++ ) 
    {
      s->dim_position[ i ] = dim_N[ i ]%dim_length[ i ];
      //printf(" dim position %d ", s->dim_position[i]);
      dim_N[ i + 1 ] = ( dim_N[ i ] - s->dim_position[ i ] )/dim_length[ i ];

      half_length[ i ] = dim_length[ i ] / 2;
    }
   //printf("\n");

  factor[ 0 ] = 1;
  for ( i=1; i < n_dims; i++ )
    {
      factor[ i ] = 1;
      for ( j = 0; j < i; j++ )
        factor[ i ] *= dim_length[ j ];
    }
  int temp_dim_pos[ n_dims ];
  for ( i = 0; i < n_dims; i++ )
    temp_dim_pos[ i ] = s->dim_position[ i ];

  tw_lpid neighbor_id;
  // calculate minus neighbour's lpID
  for ( j = 0; j < n_dims; j++ )
    {
      temp_dim_pos[ j ] = (s->dim_position[ j ] -1 + dim_length[ j ]) % dim_length[ j ];

      s->neighbour_minus_lpID[ j ] = 0;
      
      for ( i = 0; i < n_dims; i++ )
        s->neighbour_minus_lpID[ j ] += factor[ i ] * temp_dim_pos[ i ];
      
      codes_mapping_get_lp_id("MODELNET_GRP", "modelnet_torus", s->neighbour_minus_lpID[ j ], 0, &neighbor_id);
      //printf("\n neighbor %d lp id %d ", (int)s->neighbour_minus_lpID[ j ], (int)neighbor_id);
      
      temp_dim_pos[ j ] = s->dim_position[ j ];
    }
  // calculate plus neighbour's lpID
  for ( j = 0; j < n_dims; j++ )
    {
      temp_dim_pos[ j ] = ( s->dim_position[ j ] + 1 + dim_length[ j ]) % dim_length[ j ];

      s->neighbour_plus_lpID[ j ] = 0;
      
      for ( i = 0; i < n_dims; i++ )
        s->neighbour_plus_lpID[ j ] += factor[ i ] * temp_dim_pos[ i ];

      codes_mapping_get_lp_id("MODELNET_GRP", "modelnet_torus", s->neighbour_plus_lpID[ j ], 0, &neighbor_id);
      //printf("\n neighbor %d lp id %d ", (int)s->neighbour_plus_lpID[ j ], (int)neighbor_id);
      
      temp_dim_pos[ j ] = s->dim_position[ j ];
    }

  //printf("\n");
  for( j=0; j < 2 * n_dims; j++ )
   {
    for( i = 0; i < num_vc; i++ )
     {
       s->buffer[ j ][ i ] = 0; 
       s->next_link_available_time[ j ][ i ] = 0.0;
       s->next_credit_available_time[j][i] = 0.0; 
     }
   }
  // record LP time
    s->packet_counter = 0;
}

/* returns the torus message size */
static int torus_get_msg_sz(void)
{
   return sizeof(nodes_message);
}

/*Returns the next neighbor to which the packet should be routed by using DOR (Taken from Ning's code of the torus model)*/
static void dimension_order_routing( nodes_state * s,
			     tw_lpid * dst_lp, 
			     int * dim, 
			     int * dir )
{
  int dim_N[n_dims], 
      dest[n_dims],
      i,
      dest_id=0;

  codes_mapping_get_lp_info(*dst_lp, grp_name, &grp_id, &lp_type_id, type_name, &rep_id, &offset);
  dim_N[ 0 ]=rep_id + offset;

  // find destination dimensions using destination LP ID 
  for ( i = 0; i < n_dims; i++ )
    {
      dest[ i ] = dim_N[ i ] % dim_length[ i ];
      dim_N[ i + 1 ] = ( dim_N[ i ] - dest[ i ] ) / dim_length[ i ];
    }

  for( i = 0; i < n_dims; i++ )
    {
      if ( s->dim_position[ i ] - dest[ i ] > half_length[ i ] )
	{
	  dest_id = s->neighbour_plus_lpID[ i ];
	  *dim = i;
	  *dir = 1;
	  break;
	}
      if ( s->dim_position[ i ] - dest[ i ] < -half_length[ i ] )
	{
	  dest_id = s->neighbour_minus_lpID[ i ];
	  *dim = i;
	  *dir = 0;
	  break;
	}
      if ( ( s->dim_position[ i ] - dest[ i ] <= half_length[ i ] ) && ( s->dim_position[ i ] - dest[ i ] > 0 ) )
	{
	  dest_id = s->neighbour_minus_lpID[ i ];
	  *dim = i;
	  *dir = 0;
	  break;
	}
      if (( s->dim_position[ i ] - dest[ i ] >= -half_length[ i ] ) && ( s->dim_position[ i ] - dest[ i ] < 0) )
	{
	  dest_id = s->neighbour_plus_lpID[ i ];
	  *dim = i;
	  *dir = 1;
	  break;
	}
    }
  codes_mapping_get_lp_id("MODELNET_GRP", "modelnet_torus", dest_id, 0, dst_lp);
}

/*Generates a packet. If there is a buffer slot available, then the packet is 
injected in the network. Else, a buffer overflow exception is thrown.
TODO: We might want to modify this so that if the buffer is full, the packet
injection is delayed in turn slowing down the injection rate. The average achieved
injection rate can be reported at the end of the simulation. */
static void packet_generate( nodes_state * s, 
		tw_bf * bf, 
		nodes_message * msg, 
		tw_lp * lp )
{
//    printf("\n msg local event size %d remote event size %d ", msg->local_event_size_bytes, msg->remote_event_size_bytes);
    int j, tmp_dir=-1, tmp_dim=-1;
    tw_stime ts;

//    event triggered when packet head is sent
    tw_event * e_h;
    nodes_message *m;

    tw_lpid dst_lp = msg->dest_lp; 
    dimension_order_routing( s, &dst_lp, &tmp_dim, &tmp_dir );

    msg->saved_src_dim = tmp_dim;
    msg->saved_src_dir = tmp_dir;

    //msg->saved_available_time = s->next_flit_generate_time[(2*tmp_dim) + tmp_dir][0];
    msg->travel_start_time = tw_now(lp);
    msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter;

    num_chunks = msg->packet_size/CHUNK_SIZE;
    s->packet_counter++;

    if(msg->packet_ID == TRACE)
	    printf("\n packet generated %lld at lp %d dest %d final dest %d", msg->packet_ID, (int)lp->gid, (int)dst_lp, (int)msg->dest_lp);
    for(j = 0; j < num_chunks; j++)
    { 
     if(s->buffer[ tmp_dir + ( tmp_dim * 2 ) ][ 0 ] < buffer_size)
      {
       ts = j + tw_rand_exponential(lp->rng, MEAN_INTERVAL/200);
       //s->next_flit_generate_time[(2*tmp_dim) + tmp_dir][0] = max(s->next_flit_generate_time[(2*tmp_dim) + tmp_dir][0], tw_now(lp));
       //s->next_flit_generate_time[(2*tmp_dim) + tmp_dir][0] += ts;
       //e_h = tw_event_new( lp->gid, s->next_flit_generate_time[(2*tmp_dim) + tmp_dir][0] - tw_now(lp), lp);
       e_h = tw_event_new(lp->gid, ts, lp);
       msg->source_direction = tmp_dir;
       msg->source_dim = tmp_dim;

       m = tw_event_data( e_h );
       memcpy(m, msg, torus_get_msg_sz() + msg->local_event_size_bytes + msg->remote_event_size_bytes);
       m->next_stop = dst_lp;
       m->chunk_id = j;

      // find destination dimensions using destination LP ID 
       m->my_N_hop = 0;
        // Send the packet out
       m->type = SEND;
       m->source_direction = tmp_dir;
       m->source_dim = tmp_dim;
       tw_event_send(e_h);
      }
      else 
       {
   printf("\n %d Packet queued in line increase buffer space, dir %d dim %d buffer space %d dest LP %d ", (int)lp->gid, tmp_dir, tmp_dim, s->buffer[ tmp_dir + ( tmp_dim * 2 ) ][ 0 ], (int)msg->dest_lp);
       MPI_Finalize();
       exit(-1); 
       }
   }
}
/*Sends a 8-byte credit back to the torus node LP that sent the message */
static void credit_send( nodes_state * s, 
	    tw_bf * bf, 
	    tw_lp * lp, 
	    nodes_message * msg)
{
#if DEBUG
//if(lp->gid == TRACK_LP)
//	printf("\n (%lf) sending credit tmp_dir %d tmp_dim %d %lf ", tw_now(lp), msg->source_direction, msg->source_dim, credit_delay );
#endif
    bf->c1 = 0;
    tw_event * buf_e;
    nodes_message *m;
    tw_stime ts;
    int src_dir = msg->source_direction;
    int src_dim = msg->source_dim;

    msg->saved_available_time = s->next_credit_available_time[(2 * src_dim) + src_dir][0];
    s->next_credit_available_time[(2 * src_dim) + src_dir][0] = max(s->next_credit_available_time[(2 * src_dim) + src_dir][0], tw_now(lp));
    ts =  credit_delay + tw_rand_exponential(lp->rng, credit_delay/1000);
    s->next_credit_available_time[(2 * src_dim) + src_dir][0] += ts;

    buf_e = tw_event_new( msg->sender_lp, s->next_credit_available_time[(2 * src_dim) + src_dir][0] - tw_now(lp), lp);
    m = tw_event_data(buf_e);
    m->source_direction = msg->source_direction;
    m->source_dim = msg->source_dim;

    m->type = CREDIT;
    tw_event_send( buf_e );

}
// send a packet from one torus node to another torus node
// A packet can be up to 256 bytes on BG/L and BG/P and up to 512 bytes on BG/Q
static void packet_send( nodes_state * s, 
	         tw_bf * bf, 
		 nodes_message * msg, 
		 tw_lp * lp )
{ 
    bf->c2 = 0;
    bf->c1 = 0;
    int tmp_dir, tmp_dim;
    tw_stime ts;
    tw_event *e;
    nodes_message *m;
    tw_lpid dst_lp = msg->dest_lp;
    dimension_order_routing( s, &dst_lp, &tmp_dim, &tmp_dir );     

    if(s->buffer[ tmp_dir + ( tmp_dim * 2 ) ][ 0 ] < buffer_size)
    {
       bf->c2 = 1;
       msg->saved_src_dir = tmp_dir;
       msg->saved_src_dim = tmp_dim;
       ts = tw_rand_exponential( lp->rng, ( double )head_delay/200 )+ head_delay;

//    For reverse computation 
      msg->saved_available_time = s->next_link_available_time[tmp_dir + ( tmp_dim * 2 )][0];

      s->next_link_available_time[tmp_dir + ( tmp_dim * 2 )][0] = max( s->next_link_available_time[ tmp_dir + ( tmp_dim * 2 )][0], tw_now(lp) );
      s->next_link_available_time[tmp_dir + ( tmp_dim * 2 )][0] += ts;
    
      e = tw_event_new( dst_lp, s->next_link_available_time[tmp_dir + ( tmp_dim * 2 )][0] - tw_now(lp), lp );
      m = tw_event_data( e );
      memcpy(m, msg, torus_get_msg_sz() + msg->remote_event_size_bytes);
      m->type = ARRIVAL;

      if(msg->packet_ID == TRACE)
        printf("\n lp %d packet %lld flit id %d being sent to %d after time %lf ", (int) lp->gid, msg->packet_ID, msg->chunk_id, (int)dst_lp, s->next_link_available_time[tmp_dir + ( tmp_dim * 2 )][0] - tw_now(lp)); 
      //Carry on the message info
      m->source_dim = tmp_dim;
      m->source_direction = tmp_dir;
      m->next_stop = dst_lp;
      m->sender_lp = lp->gid;
      m->local_event_size_bytes = 0; /* We just deliver the local event here */

      tw_event_send( e );

      s->buffer[ tmp_dir + ( tmp_dim * 2 ) ][ 0 ]++;
    
      if(msg->chunk_id == num_chunks - 1)
      {
        bf->c1 = 1;
	if(msg->local_event_size_bytes > 0)
	{
          tw_event* e_new;
	  nodes_message* m_new;
	  char* local_event;
	  ts = (1/link_bandwidth) * msg->local_event_size_bytes;
	  e_new = codes_event_new(msg->sender_lp, ts, lp);
	  m_new = tw_event_data(e_new);
	  local_event = (char*)msg;
	  local_event += torus_get_msg_sz() + msg->remote_event_size_bytes;
	  memcpy(m_new, local_event, msg->local_event_size_bytes);
	  tw_event_send(e_new);
	}
     }
  } // end if
    else
    {
	    printf("\n buffer overflown ");
	    MPI_Finalize();
	    exit(-1);
    }
}

/*Processes the packet after it arrives from the neighboring torus node 
 * routes it to the next compute node if this is not the destination
 * OR if this is the destination then a remote event at the server is issued. */
static void packet_arrive( nodes_state * s, 
		    tw_bf * bf, 
		    nodes_message * msg, 
		    tw_lp * lp )
{
  bf->c2 = 0;
  tw_event *e;
  tw_stime ts;
  nodes_message *m;

  credit_send( s, bf, lp, msg); // Commented on May 22nd to check if the credit needs to be sent from the final destination or not
  
  msg->my_N_hop++;
  ts = 0.1 + tw_rand_exponential(lp->rng, MEAN_INTERVAL/200);
  if(msg->packet_ID == TRACE)
	  printf("\n packet arrived at lp %d final dest %d ", (int)lp->gid, (int)msg->dest_lp);
  if( lp->gid == msg->dest_lp )
    {   
        if( msg->chunk_id == num_chunks - 1 )    
        {
	    bf->c2 = 1;
	    N_finished_packets++;
	    total_time += tw_now( lp ) - msg->travel_start_time;
	    total_hops += msg->my_N_hop;

	    if (max_latency < tw_now( lp ) - msg->travel_start_time) {
		  bf->c3 = 1;
		  msg->saved_available_time = max_latency;
	          max_latency=tw_now( lp ) - msg->travel_start_time;
     		}
	    // Trigger an event on receiving server
	    if(msg->remote_event_size_bytes)
	    {
	       ts = (1/link_bandwidth) * msg->remote_event_size_bytes;
	       if(msg->packet_ID == TRACE)
		     printf("\n lp %d Sending final message after %lf ",(int)lp->gid, ts);
	       e = codes_event_new(msg->final_dest_gid, ts, lp);
	       m = tw_event_data(e);
	       char* tmp_ptr = (char*)msg;
	       tmp_ptr += torus_get_msg_sz();
	       memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
	       tw_event_send(e);
	    }
       }
    }
  else
    {
      e = tw_event_new(lp->gid, ts , lp);
      m = tw_event_data( e );
      memcpy(m, msg, torus_get_msg_sz() + msg->remote_event_size_bytes);
      m->type = SEND;
      m->next_stop = -1;
      tw_event_send(e);
   }
}

static void torus_report_stats()
{
    long long avg_hops, total_finished_packets;
    tw_stime avg_time, max_time;

    MPI_Reduce( &total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &total_time, &avg_time, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    if(!g_tw_mynode)
     {
       printf(" Average number of hops traversed %f average message latency %lf us maximum message latency %lf us \n", (float)avg_hops/total_finished_packets, avg_time/(total_finished_packets*1000), max_time/1000);
     }
}
void
final( nodes_state * s, tw_lp * lp )
{
  free(s->next_link_available_time);
  free(s->next_credit_available_time);
  free(s->next_flit_generate_time);
  free(s->buffer); 
}

/* increments the buffer count after a credit arrives from the remote compute node */
static void packet_buffer_process( nodes_state * s, tw_bf * bf, nodes_message * msg, tw_lp * lp )
{
   s->buffer[ msg->source_direction + ( msg->source_dim * 2 ) ][  0 ]--;
}

/* reverse handler */
static void node_rc_handler(nodes_state * s, tw_bf * bf, nodes_message * msg, tw_lp * lp)
{
  switch(msg->type)
    {
       case GENERATE:
		   {
		     s->packet_counter--;
		     int i;//, saved_dim, saved_dir;
	 	     //saved_dim = msg->saved_src_dim;
		     //saved_dir = msg->saved_src_dir;

		     //s->next_flit_generate_time[(saved_dim * 2) + saved_dir][0] = msg->saved_available_time;
		     for(i=0; i < num_chunks; i++)
  		        tw_rand_reverse_unif(lp->rng);	
		   }
	break;
	
	case ARRIVAL:
		   {
 		    msg->my_N_hop--;
  		    tw_rand_reverse_unif(lp->rng);
		    tw_rand_reverse_unif(lp->rng);
		    int next_dim = msg->source_dim;
		    int next_dir = msg->source_direction;

		    s->next_credit_available_time[next_dir + ( next_dim * 2 )][0] = msg->saved_available_time;
		    if(bf->c2)
		    {
		       N_finished_packets--;
		       total_time -= tw_now( lp ) - msg->travel_start_time;
		       total_hops -= msg->my_N_hop;
		    }
		   }
	break;	

	case SEND:
		 {
		    if(bf->c2)
		     {
                        int next_dim = msg->saved_src_dim;
			int next_dir = msg->saved_src_dir;
			s->next_link_available_time[next_dir + ( next_dim * 2 )][0] = msg->saved_available_time;
			s->buffer[ next_dir + ( next_dim * 2 ) ][ 0 ] --;
	                tw_rand_reverse_unif(lp->rng);
		    }
		 }
	break;

       case CREDIT:
		{
		  s->buffer[ msg->source_direction + ( msg->source_dim * 2 ) ][  0 ]++;
              }
       break;
     }
}

/* forward event handler */
static void event_handler(nodes_state * s, tw_bf * bf, nodes_message * msg, tw_lp * lp)
{
 *(int *) bf = (int) 0;
 switch(msg->type)
 {
  case GENERATE:
    packet_generate(s,bf,msg,lp);
  break;
  case ARRIVAL:
    packet_arrive(s,bf,msg,lp);
  break;
  case SEND:
   packet_send(s,bf,msg,lp);
  break;
  case CREDIT:
    packet_buffer_process(s,bf,msg,lp);
   break;
  default:
	printf("\n Being sent to wrong LP");
  break;
 }
}

tw_lptype torus_lp =
{
  (init_f) torus_init,
  (event_f) event_handler,
  (revent_f) node_rc_handler,
  (final_f) final,
  (map_f) codes_mapping,
  sizeof(nodes_state),
};

/* returns the torus lp type for lp registration */
static const tw_lptype* torus_get_lp_type(void)
{
   return(&torus_lp); 
}

