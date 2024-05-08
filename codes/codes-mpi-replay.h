/*
 * Copyright (C) 2017 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */


#ifndef CODES_MPI_REPLAY_H
#define CODES_MPI_REPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <mpi.h>
#include <codes/model-net.h>
#include <codes/codes-workload.h>


int modelnet_mpi_replay(MPI_Comm comm, int* argc, char*** argv );


/* MPI_OP_GET_NEXT is for getting next MPI operation when the previous operation completes.
* MPI_SEND_ARRIVED is issued when a MPI message arrives at its destination (the message is transported by model-net and an event is invoked when it arrives.
* MPI_SEND_POSTED is issued when a MPI message has left the source LP (message is transported via model-net). */
enum MPI_NW_EVENTS
{
    MPI_OP_GET_NEXT=1,
    MPI_SEND_ARRIVED,
    MPI_SEND_ARRIVED_CB, // for tracking message times on sender
    MPI_SEND_POSTED,
    MPI_REND_ARRIVED,
    MPI_REND_ACK_ARRIVED,
    CLI_BCKGND_FIN,
    CLI_BCKGND_ARRIVE,
    CLI_BCKGND_GEN,
    CLI_BCKGND_CHANGE,
    CLI_NBR_FINISH,
    CLI_OTHER_FINISH, //received when another workload has finished
    // Surrogate-mediated events
    SURR_START_NEXT_ITERATION, // tells the workload to continue processing events as normal for the next iteration
    SURR_SKIP_ITERATION, // skips one (several) iteration(s) of simulation
};


/* data for handling reverse computation.
* saved_matched_req holds the request ID of matched receives/sends for wait operations.
* ptr_match_op holds the matched MPI operation which are removed from the queues when a send is matched with the receive in forward event handler.
* network event being sent. op is the MPI operation issued by the network workloads API. rv_data holds the data for reverse computation (TODO: Fill this data structure only when the simulation runs in optimistic mode). */
struct nw_message
{
   // forward message handler
   enum MPI_NW_EVENTS msg_type;
   int op_type;
   int num_rngs;
   model_net_event_return event_rc;
   struct codes_workload_op * mpi_op;

   struct
   {
       tw_lpid src_rank;
       int dest_rank;
       int64_t num_bytes;
       int num_matched;
       int data_type;
       double sim_start_time;
       // for callbacks - time message was received
       double msg_send_time;
       unsigned int req_id;
       int matched_req;
       int tag;
       int app_id;
       int found_match;
       short wait_completed;
       short rend_send;
       int skip_iterations; // Number of iterations to jump ahead
   } fwd;
   struct
   {
       int saved_perm;
       double saved_send_time;
       double saved_send_time_sample;
       double saved_recv_time;
       double saved_recv_time_sample;
       double saved_wait_time;
       double saved_wait_time_sample;
       double saved_delay;
       double saved_delay_sample;
       double saved_marker_time;
       int64_t saved_num_bytes;
       int saved_syn_length;
       unsigned long saved_prev_switch;
       double saved_prev_max_time;
   } rc;
};


#ifdef __cplusplus
}
#endif

#endif /* CODES_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
