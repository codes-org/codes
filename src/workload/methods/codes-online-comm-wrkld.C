/*
 * Copyright (C) 2014 University of Chicago
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <ross.h>
#include <assert.h>
#include <deque>
#include <iostream>
#include <inttypes.h>
#include <fstream>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include "codes/codes-workload.h"
#include "codes/quickhash.h"
#include "codes/codes-jobmap.h"
#include "codes_config.h"
#include "lammps.h"
#include "nekbone_swm_user_code.h"
#include "nearest_neighbor_swm_user_code.h"
#include "all_to_one_swm_user_code.h"
#include "one_to_many_swm_user_code.h"
#include "many_to_many_swm_user_code.h"
#include "milc_swm_user_code.h"
#include "allreduce.h"
#include "periodic_aggressor.h"

#define ALLREDUCE_SHORT_MSG_SIZE 2048


//#define DBG_COMM 0

using namespace std;

static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;
static int total_rank_cnt = 0;
ABT_thread global_prod_thread = NULL;
ABT_xstream self_es;
double cpu_freq = 1.0;
long num_allreduce = 0;
long num_isends = 0;
long num_irecvs = 0;
long num_barriers = 0;
long num_sends = 0;
long num_recvs = 0;
long num_sendrecv = 0;
long num_waitalls = 0;

//std::map<int64_t, int> send_count;
//std::map<int64_t, int> isend_count;
//std::map<int64_t, int> allreduce_count;

struct shared_context {
    int my_rank;
    uint32_t wait_id;
    int num_ranks;
    char workload_name[MAX_NAME_LENGTH_WKLD];
    void * swm_obj;
    ABT_thread      producer;
    std::deque<struct codes_workload_op*> fifo;
};

struct rank_mpi_context {
    struct qhash_head hash_link;
    int app_id;
    struct shared_context sctx;
};

typedef struct rank_mpi_compare {
    int app_id;
    int rank;
} rank_mpi_compare;

/*
 * peer: the receiving peer id 
 * comm_id: the communicator id being used
 * tag: tag id 
 * reqvc: virtual channel being used by the message (to be ignored)
 * rspvc: virtual channel being used by the message (to be ignored)
 * buf: the address of sender's buffer in memory
 * bytes: number of bytes to be sent 
 * reqrt and rsprt: routing types (to be ignored) */

void SWM_Send(SWM_PEER peer,
        SWM_COMM_ID comm_id,
        SWM_TAG tag,
        SWM_VC reqvc,
        SWM_VC rspvc,
        SWM_BUF buf,
        SWM_BYTES bytes,
        SWM_BYTES pktrspbytes,
        SWM_ROUTING_TYPE reqrt,
        SWM_ROUTING_TYPE rsprt)
{
    /* add an event in the shared queue and then yield */
    //    printf("\n Sending to rank %d ", comm_id);
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_SEND;
    wrkld_per_rank.u.send.tag = tag;
    wrkld_per_rank.u.send.num_bytes = bytes;
    wrkld_per_rank.u.send.dest_rank = peer;

#ifdef DBG_COMM
/*    if(tag != 1235 && tag != 1234) 
    {
        auto it = send_count.find(bytes);
        if(it == send_count.end())
        {
            send_count.insert(std::make_pair(bytes, 1));
        }
        else
        {
            it->second = it->second + 1;
        }
    }*/
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    wrkld_per_rank.u.send.source_rank = sctx->my_rank;
    sctx->fifo.push_back(&wrkld_per_rank);

    ABT_thread_yield_to(global_prod_thread);
    num_sends++;
}

/*
 * @param comm_id: communicator ID (For now, MPI_COMM_WORLD)
 * reqvc and rspvc: virtual channel IDs for request and response (ignore for
 * our purpose)
 * buf: buffer location for the call (ignore for our purpose)
 * reqrt and rsprt: routing types, ignore and use routing from config file instead. 
 * */
void SWM_Barrier(
        SWM_COMM_ID comm_id,
        SWM_VC reqvc,
        SWM_VC rspvc,
        SWM_BUF buf, 
        SWM_UNKNOWN auto1,
        SWM_UNKNOWN2 auto2,
        SWM_ROUTING_TYPE reqrt, 
        SWM_ROUTING_TYPE rsprt)
{
    /* Add an event in the shared queue and then yield */
#if 0
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_DELAY;
    /* TODO: Check how to convert cycle count into delay? */
    wrkld_per_rank.u.delay.nsecs = 0.1;

#ifdef DBG_COMM
    printf("\n Barrier delay %lf ", wrkld_per_rank.u.delay.nsecs);
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&wrkld_per_rank);

    ABT_thread_yield_to(global_prod_thread);
#endif
#ifdef DBG_COMM
//     printf("\n barrier ");
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err;
    int rank, size, src, dest, mask;

    err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);

    rank = sctx->my_rank;
    size = sctx->num_ranks;
    mask = 0x1;

    while(mask < size) {
        dest = (rank + mask) % size;
        src = (rank - mask + size) % size;

        SWM_Sendrecv(comm_id, dest, 1234, reqvc, rspvc, 0, 0, 0,
                src,  1234, 0,  reqrt, rsprt);
        mask <<= 1;
    }
    num_barriers++;
}

void SWM_Isend(SWM_PEER peer,
        SWM_COMM_ID comm_id,
        SWM_TAG tag,
        SWM_VC reqvc,
        SWM_VC rspvc,
        SWM_BUF buf,
        SWM_BYTES bytes,
        SWM_BYTES pktrspbytes,
        uint32_t * handle,
        SWM_ROUTING_TYPE reqrt,
        SWM_ROUTING_TYPE rsprt)
{
    /* add an event in the shared queue and then yield */
    //    printf("\n Sending to rank %d ", comm_id);
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_ISEND;
    wrkld_per_rank.u.send.tag = tag;
    wrkld_per_rank.u.send.num_bytes = bytes;
    wrkld_per_rank.u.send.dest_rank = peer;

#ifdef DBG_COMM
/*    if(tag != 1235 && tag != 1234) 
    {
        auto it = isend_count.find(bytes);
        if(it == isend_count.end())
        {
            isend_count.insert(std::make_pair(bytes, 1));
        }
        else
        {
            it->second = it->second + 1;
        }
    }*/
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    wrkld_per_rank.u.send.source_rank = sctx->my_rank;
    sctx->fifo.push_back(&wrkld_per_rank);

    *handle = sctx->wait_id;
    wrkld_per_rank.u.send.req_id = *handle;
    sctx->wait_id++;

    ABT_thread_yield_to(global_prod_thread);
    num_isends++;
}
void SWM_Recv(SWM_PEER peer,
        SWM_COMM_ID comm_id,
        SWM_TAG tag,
        SWM_BUF buf)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_RECV;
    wrkld_per_rank.u.recv.tag = tag;
    wrkld_per_rank.u.recv.source_rank = peer;
    wrkld_per_rank.u.recv.num_bytes = 0;

#ifdef DBG_COMM
    //printf("\n recv op tag: %d source: %d ", tag, peer);
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    wrkld_per_rank.u.recv.dest_rank = sctx->my_rank;
    sctx->fifo.push_back(&wrkld_per_rank);

    ABT_thread_yield_to(global_prod_thread);
    num_recvs++;
}

/* handle is for the request ID */
void SWM_Irecv(SWM_PEER peer,
        SWM_COMM_ID comm_id,
        SWM_TAG tag,
        SWM_BUF buf, 
        uint32_t* handle)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_IRECV;
    wrkld_per_rank.u.recv.tag = tag;
    wrkld_per_rank.u.recv.source_rank = peer;
    wrkld_per_rank.u.recv.num_bytes = 0;

#ifdef DBG_COMM
//    printf("\n irecv op tag: %d source: %d ", tag, peer);
#endif

    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    wrkld_per_rank.u.recv.dest_rank = sctx->my_rank;
    sctx->fifo.push_back(&wrkld_per_rank);
    
    *handle = sctx->wait_id;
    wrkld_per_rank.u.recv.req_id = *handle;
    sctx->wait_id++;

    ABT_thread_yield_to(global_prod_thread);
    num_irecvs++;
}

void SWM_Compute(long cycle_count)
{
    //NM: noting that cpu_frequency has been loaded in comm_online_workload_load() as GHz, e.g. cpu_freq = 2.0 means 2.0GHz
    if(!cpu_freq)
        cpu_freq = 2.0;
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    double cpu_freq_hz = cpu_freq * 1000.0 * 1000.0 * 1000.0;
    double delay_in_seconds = cycle_count / cpu_freq_hz;
    double delay_in_ns = delay_in_seconds * 1000.0 * 1000.0 * 1000.0;

    wrkld_per_rank.op_type = CODES_WK_DELAY;
    /* TODO: Check how to convert cycle count into delay? */
    wrkld_per_rank.u.delay.nsecs = delay_in_ns;
    wrkld_per_rank.u.delay.seconds = delay_in_seconds;
#ifdef DBG_COMM
    printf("\n compute op delay: %ld ", delay_in_ns);
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&wrkld_per_rank);
	
    ABT_thread_yield_to(global_prod_thread);

}

void SWM_Wait(uint32_t req_id)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_WAIT;
    /* TODO: Check how to convert cycle count into delay? */
    wrkld_per_rank.u.wait.req_id = req_id;

#ifdef DBG_COMM
//    printf("\n wait op req_id: %"PRIu32"\n", req_id);
//      printf("\n wait ");
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&wrkld_per_rank);

    ABT_thread_yield_to(global_prod_thread);
}

void SWM_Waitall(int len, uint32_t * req_ids)
{
    num_waitalls++;
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_WAITALL;
    /* TODO: Check how to convert cycle count into delay? */
    wrkld_per_rank.u.waits.count = len;
    wrkld_per_rank.u.waits.req_ids = (unsigned int*)calloc(len, sizeof(int));    

    for(int i = 0; i < len; i++)
        wrkld_per_rank.u.waits.req_ids[i] = req_ids[i];

#ifdef DBG_COMM
//    for(int i = 0; i < len; i++)
//        printf("\n wait op len %d req_id: %"PRIu32"\n", len, req_ids[i]);
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&wrkld_per_rank);

    ABT_thread_yield_to(global_prod_thread);
}

void SWM_Sendrecv(
        SWM_COMM_ID comm_id,
        SWM_PEER sendpeer,
        SWM_TAG sendtag,
        SWM_VC sendreqvc,
        SWM_VC sendrspvc,
        SWM_BUF sendbuf,
        SWM_BYTES sendbytes,
        SWM_BYTES pktrspbytes,
        SWM_PEER recvpeer,
        SWM_TAG recvtag,
        SWM_BUF recvbuf,
        SWM_ROUTING_TYPE reqrt,
        SWM_ROUTING_TYPE rsprt)
{

#if 1
// Alternate, simpler implementation. Matches MPICH design. OpenMPI does Irecv+Send which also works.
    uint32_t handles[2];
    SWM_Isend(sendpeer, comm_id, sendtag, sendreqvc, sendrspvc, sendbuf, sendbytes, pktrspbytes, &handles[0], reqrt, rsprt);
    SWM_Irecv(recvpeer, comm_id, recvtag, recvbuf, &handles[1]);
    SWM_Waitall(2, handles);
#else
    //    printf("\n Sending to %d receiving from %d ", sendpeer, recvpeer);
    struct codes_workload_op send_op;

    send_op.op_type = CODES_WK_SEND;
    send_op.u.send.tag = sendtag;
    send_op.u.send.num_bytes = sendbytes;
    send_op.u.send.dest_rank = sendpeer;

    /* Add an event in the shared queue and then yield */
    struct codes_workload_op recv_op;

    recv_op.op_type = CODES_WK_RECV;
    recv_op.u.recv.tag = recvtag;
    recv_op.u.recv.source_rank = recvpeer;
    recv_op.u.recv.num_bytes = 0;

#ifdef DBG_COMM
/*    if(sendtag != 1235 && sendtag != 1234) 
    {
        auto it = send_count.find(sendbytes);
        if(it == send_count.end())
        {
            send_count.insert(std::make_pair(sendbytes, 1));
        }
        else
        {
            it->second = it->second + 1;
        }
    }*/
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    recv_op.u.recv.dest_rank = sctx->my_rank;
    send_op.u.send.source_rank = sctx->my_rank;
    sctx->fifo.push_back(&send_op);
    sctx->fifo.push_back(&recv_op);

    ABT_thread_yield_to(global_prod_thread);
    num_sendrecv++;
#endif

}

/* @param count: number of bytes in Allreduce
 * @param respbytes: number of bytes to be sent in response (ignore for our
 * purpose)
 * $params comm_id: communicator ID (MPI_COMM_WORLD for our case)
 * @param sendreqvc: virtual channel of the sender request (ignore for our
 * purpose)
 * @param sendrspvc: virtual channel of the response request (ignore for our
 * purpose)
 * @param sendbuf and rcvbuf: buffers for send and receive calls (ignore for
 * our purpose) */
void SWM_Allreduce(
        SWM_BYTES count,
        SWM_BYTES respbytes,
        SWM_COMM_ID comm_id,
        SWM_VC sendreqvc,
        SWM_VC sendrspvc,
        SWM_BUF sendbuf,
        SWM_BUF rcvbuf)
{
#if 0
    /* TODO: For now, simulate a constant delay for ALlreduce*/
    //    printf("\n Allreduce bytes %d ", bytes);
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_DELAY;
    /* TODO: Check how to convert cycle count into delay? */
    wrkld_per_rank.u.delay.nsecs = bytes + 0.1;

#ifdef DBG_COMM
    printf("\n Allreduce delay %lf ", wrkld_per_rank.u.delay.nsecs);
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&wrkld_per_rank);

    ABT_thread_yield_to(global_prod_thread);
#endif

#ifdef DBG_COMM
        auto it = allreduce_count.find(count);
        if(it == allreduce_count.end())
        {
            allreduce_count.insert(std::make_pair(count, 1));
        }
        else
        {
            it->second = it->second + 1;
        }
#endif
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);

    int comm_size, i, send_idx, recv_idx, last_idx, send_cnt, recv_cnt;
    int pof2, mask, rem, newrank, newdst, dst, *cnts, *disps;
    int rank = sctx->my_rank;
    comm_size = sctx->num_ranks;

    cnts = disps = NULL;

    pof2 = 1;
    while (pof2 <= comm_size) pof2 <<= 1;
    pof2 >>=1;

    rem = comm_size - pof2;

    /* In the non-power-of-two case, all even-numbered
       processes of rank < 2*rem send their data to
       (rank+1). These even-numbered processes no longer
       participate in the algorithm until the very end. The
       remaining processes form a nice power-of-two. */
    if (rank < 2*rem) {
        if (rank % 2 == 0) { /* even */
            SWM_Send(rank+1, comm_id, 1235, sendreqvc, sendrspvc, 0, count, 1, 0, 0);
            newrank = -1;
        } else { /* odd */
            SWM_Recv(rank-1, comm_id, 1235, 0);
            newrank = rank / 2;
        }
    } else {
        newrank = rank - rem;
    }

    /* If op is user-defined or count is less than pof2, use
       recursive doubling algorithm. Otherwise do a reduce-scatter
       followed by allgather. (If op is user-defined,
       derived datatypes are allowed and the user could pass basic
       datatypes on one process and derived on another as long as
       the type maps are the same. Breaking up derived
       datatypes to do the reduce-scatter is tricky, therefore
       using recursive doubling in that case.) */
    if (newrank != -1) {
        if ((count <= ALLREDUCE_SHORT_MSG_SIZE) || (count < pof2)) {

            mask = 0x1;
            while (mask < pof2) {
                newdst = newrank ^ mask;
                dst = (newdst < rem) ? newdst*2 + 1 : newdst + rem;

                SWM_Sendrecv(comm_id, dst, 1235, sendreqvc, sendrspvc, 0,
                        count, 1, dst, 1235, 0, 0, 0);

                mask <<= 1;
            }
        } else {
            /* do a reduce-scatter followed by allgather */
            /* for the reduce-scatter, calculate the count that
               each process receives and the displacement within
               the buffer */

            cnts = (int*)malloc(pof2*sizeof(int));
            disps = (int*)malloc(pof2*sizeof(int));

            for (i=0; i<(pof2-1); i++)
                cnts[i] = count/pof2;
            cnts[pof2-1] = count - (count/pof2)*(pof2-1);

            disps[0] = 0;
            for (i=1; i<pof2; i++)
                disps[i] = disps[i-1] + cnts[i-1];

            mask = 0x1;
            send_idx = recv_idx = 0;
            last_idx = pof2;
            while (mask < pof2) {
                newdst = newrank ^ mask;
                dst = (newdst < rem) ? newdst*2 + 1 : newdst + rem;
                send_cnt = recv_cnt = 0;
                if (newrank < newdst) {
                    send_idx = recv_idx + pof2/(mask*2);
                    for (i=send_idx; i<last_idx; i++)
                        send_cnt += cnts[i];
                    for (i=recv_idx; i<send_idx; i++)
                        recv_cnt += cnts[i];
                } else {
                    recv_idx = send_idx + pof2/(mask*2);
                    for (i=send_idx; i<recv_idx; i++)
                        send_cnt += cnts[i];
                    for (i=recv_idx; i<last_idx; i++)
                        recv_cnt += cnts[i];
                }

                SWM_Sendrecv(comm_id, dst, 1235, sendreqvc, sendrspvc, 0,
                        send_cnt, 1, dst, 1235, 0, 0, 0);

                send_idx = recv_idx;
                mask <<= 1;

                if(mask < pof2)
                    last_idx = recv_idx + pof2/mask;
            }

            /* now do the allgather */
            mask >>= 1;
            while (mask > 0) {
                newdst = newrank ^ mask;
                /* find real rank of dest */
                dst = (newdst < rem) ? newdst*2 + 1 : newdst + rem;

                send_cnt = recv_cnt = 0;
                if (newrank < newdst) {
                    if (mask != pof2/2)
                        last_idx = last_idx + pof2/(mask*2);

                    recv_idx = send_idx + pof2/(mask*2);
                    for (i=send_idx; i<recv_idx; i++)
                        send_cnt += cnts[i];
                    for (i=recv_idx; i<last_idx; i++)
                        recv_cnt += cnts[i];
                } else {
                    recv_idx = send_idx - pof2/(mask*2);
                    for (i=send_idx; i<last_idx; i++)
                        send_cnt += cnts[i];
                    for (i=recv_idx; i<send_idx; i++)
                        recv_cnt += cnts[i];
                }

                SWM_Sendrecv(comm_id, dst, 1235, sendreqvc, sendrspvc, 0,
                        send_cnt, 1, dst, 1235, 0, 0, 0);

                if (newrank > newdst) send_idx = recv_idx;

                mask >>= 1;
            }
        }
    }

    if(rank < 2*rem) {
        if(rank % 2) {/* odd */
            SWM_Send(rank-1, comm_id, 1235, sendreqvc, sendrspvc, 0, count, 1, 0, 0);
        } else {
            SWM_Recv(rank+1, comm_id, 1235, 0);
        }
    }

    if(cnts) free(cnts);
    if(disps) free(disps);

    num_allreduce++;
}

void SWM_Allreduce(
        SWM_BYTES bytes,
        SWM_BYTES respbytes,
        SWM_COMM_ID comm_id,
        SWM_VC sendreqvc,
        SWM_VC sendrspvc,
        SWM_BUF sendbuf,
        SWM_BUF rcvbuf,
        SWM_UNKNOWN auto1,
        SWM_UNKNOWN2 auto2,
        SWM_ROUTING_TYPE reqrt,
        SWM_ROUTING_TYPE rsprt)
{
    SWM_Allreduce(bytes, respbytes, comm_id, sendreqvc, sendrspvc, sendbuf, rcvbuf);
}

void SWM_Finalize()
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_END;

    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&wrkld_per_rank);

#ifdef DBG_COMM 
/*    auto it = allreduce_count.begin();
    for(; it != allreduce_count.end(); it++)
    {
        cout << "\n Allreduce " << it->first << " " << it->second;
    }
    
    it = send_count.begin();
    for(; it != send_count.end(); it++)
    {
        cout << "\n Send " << it->first << " " << it->second;
    }
    
    it = isend_count.begin();
    for(; it != isend_count.end(); it++)
    {
        cout << "\n isend " << it->first << " " << it->second;
    }*/
#endif
//#ifdef DBG_COMM
//    printf("\n finalize workload for rank %d ", sctx->my_rank);
//    printf("\n finalize workload for rank %d num_sends %d num_recvs %d num_isends %d num_irecvs %d num_allreduce %d num_barrier %d num_waitalls %d", sctx->my_rank, num_sends, num_recvs, num_isends, num_irecvs, num_allreduce, num_barriers, num_waitalls);
//#endif
    ABT_thread_yield_to(global_prod_thread);
}

void SWM_Mark_Iteration(SWM_TAG iter_tag)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_MARK;
    wrkld_per_rank.u.send.tag = iter_tag;

    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    wrkld_per_rank.u.send.source_rank = sctx->my_rank;
    sctx->fifo.push_back(&wrkld_per_rank);

    ABT_thread_yield_to(global_prod_thread);
}

static int hash_rank_compare(void *key, struct qhash_head *link)
{
    rank_mpi_compare *in = (rank_mpi_compare*)key;
    rank_mpi_context *tmp;

    tmp = qhash_entry(link, rank_mpi_context, hash_link);
    if (tmp->sctx.my_rank == in->rank && tmp->app_id == in->app_id)
        return 1;
    return 0;
}
static void workload_caller(void * arg)
{
    shared_context* sctx = static_cast<shared_context*>(arg);

    if(strcmp(sctx->workload_name, "lammps") == 0 || strcmp(sctx->workload_name, "lammps1") == 0)
    {
        LAMMPS_SWM * lammps_swm = static_cast<LAMMPS_SWM*>(sctx->swm_obj);
        lammps_swm->call();
    }
    else if(strcmp(sctx->workload_name, "nekbone") == 0 || strcmp(sctx->workload_name, "nekbone1") == 0)
    {
        NEKBONESWMUserCode * nekbone_swm = static_cast<NEKBONESWMUserCode*>(sctx->swm_obj);
        nekbone_swm->call();
    }
    else if(strcmp(sctx->workload_name, "nearest_neighbor") == 0)
    {
       NearestNeighborSWMUserCode * nn_swm = static_cast<NearestNeighborSWMUserCode*>(sctx->swm_obj);
       nn_swm->call();
    }
    else if(strcmp(sctx->workload_name, "incast") == 0 || strcmp(sctx->workload_name, "incast1") == 0 || strcmp(sctx->workload_name, "incast2") == 0)
    {
       AllToOneSWMUserCode * incast_swm = static_cast<AllToOneSWMUserCode*>(sctx->swm_obj);
       incast_swm->call();
    }
    else if(strcmp(sctx->workload_name, "spread") == 0)
    {
        OneToManySWMUserCode * spread_swm = static_cast< OneToManySWMUserCode*>(sctx->swm_obj);
        spread_swm->call();
    }
    else if(strcmp(sctx->workload_name, "allreduce") == 0 || strcmp(sctx->workload_name, "allreduce32") == 0 || strcmp(sctx->workload_name, "allreduce256") == 0)
    {
        AllReduceSWMUserCode * allreduce_swm = static_cast< AllReduceSWMUserCode*>(sctx->swm_obj);
        allreduce_swm->call();
    }
    else if(strcmp(sctx->workload_name, "many_to_many") == 0 || strcmp(sctx->workload_name, "many_to_many1") == 0)
    {
        ManyToManySWMUserCode * many_to_many_swm = static_cast< ManyToManySWMUserCode*>(sctx->swm_obj);
        many_to_many_swm->call();
    }
    else if(strcmp(sctx->workload_name, "milc") == 0)
    {
        MilcSWMUserCode * milc_swm = static_cast< MilcSWMUserCode*>(sctx->swm_obj);
        milc_swm->call();
    }
    else if(strcmp(sctx->workload_name, "periodic_aggressor") == 0)
    {
        PeriodicAggressor * periodic_aggressor_swm = static_cast<PeriodicAggressor*>(sctx->swm_obj);
        periodic_aggressor_swm->call();
    }
}

string get_default_path(online_comm_params * o_params)
{
    string path;
    path.append(SWM_DATAROOTDIR);

    if(strcmp(o_params->workload_name, "lammps") == 0)
    {
        path.append("/lammps_workload.json");
    }
    else if(strcmp(o_params->workload_name, "lammps1") == 0)
    {
        path.append("/lammps_workload1.json");
    }
    else if(strcmp(o_params->workload_name, "nekbone") == 0)
    {
        path.append("/workload.json");
    }
    else if(strcmp(o_params->workload_name, "nekbone1") == 0)
    {
        path.append("/workload1.json");
    }
    else if(strcmp(o_params->workload_name, "nearest_neighbor") == 0)
    {
        path.append("/skeleton.json"); 
    }
    else if(strcmp(o_params->workload_name, "incast") == 0)
    {
        path.append("/incast.json"); 
    }
    else if(strcmp(o_params->workload_name, "incast1") == 0)
    {
        path.append("/incast1.json"); 
    }
    else if(strcmp(o_params->workload_name, "incast2") == 0)
    {
        path.append("/incast2.json"); 
    }
    else if(strcmp(o_params->workload_name, "spread") == 0)
    {
        path.append("/spread_workload.json");
    }
    else if(strcmp(o_params->workload_name, "allreduce") == 0)
    {
        path.append("/allreduce_workload.json");
    }
    else if(strcmp(o_params->workload_name, "allreduce32") == 0)
    {
        path.append("/allreduce32_workload.json");
    }
    else if(strcmp(o_params->workload_name, "allreduce256") == 0)
    {
        path.append("/allreduce256_workload.json");
    }
    else if(strcmp(o_params->workload_name, "many_to_many") == 0)
    {
        path.append("/many_to_many_workload.json");
    }
    else if(strcmp(o_params->workload_name, "many_to_many1") == 0)
    {
        path.append("/many_to_many_workload1.json");
    }
    else if(strcmp(o_params->workload_name, "milc") == 0)
    {
        path.append("/milc_skeleton.json");
    }
    else if(strcmp(o_params->workload_name, "periodic_aggressor") == 0)
    {
        path.append("/periodic_aggressor.json");
    }
    else
        tw_error(TW_LOC, "\n Undefined workload type %s ", o_params->workload_name);

    return path;
}


static int comm_online_workload_load(const char * params, int app_id, int rank)
{
    /* LOAD parameters from JSON file*/
    online_comm_params * o_params = (online_comm_params*)params;
    int nprocs = o_params->nprocs;

    rank_mpi_context *my_ctx = new rank_mpi_context;
    //my_ctx = (rank_mpi_context*)caloc(1, sizeof(rank_mpi_context));
    assert(my_ctx);
    my_ctx->sctx.my_rank = rank;
    my_ctx->sctx.num_ranks = nprocs;
    my_ctx->sctx.wait_id = 0;
    my_ctx->app_id = app_id;

    void** generic_ptrs;
    int array_len = 1;
    generic_ptrs = (void**)calloc(array_len,  sizeof(void*));
    generic_ptrs[0] = (void*)&rank;

    string path;

    if (o_params->workload_name[0] != '\0') { //then we were supplied with just the workload name, use default configs
        strcpy(my_ctx->sctx.workload_name, o_params->workload_name);
        path = get_default_path(o_params);
    }
    else { //then we were supplied a filepath to the config in the workload conf file
        path = std::string(o_params->file_path);
    }

    boost::property_tree::ptree root;
    try {
        std::ifstream jsonFile(path.c_str());
        boost::property_tree::json_parser::read_json(jsonFile, root);
        uint32_t process_cnt = root.get<uint32_t>("jobs.size", 1);
        cpu_freq = root.get<double>("jobs.cfg.cpu_freq") / 1e9;
        if (o_params->workload_name[0] == '\0') //if we instead had a configuration filename supplied, get worklaod name from jobs.cfg.app
            strcpy(o_params->workload_name, root.get<string>("jobs.cfg.app").c_str());
    }
    catch(std::exception & e)
    {
        printf("%s \n", e.what());
        return -1;
    }
    if(strcmp(o_params->workload_name, "lammps") == 0 || strcmp(o_params->workload_name, "lammps1") == 0)
    {
        LAMMPS_SWM * lammps_swm = new LAMMPS_SWM(root, generic_ptrs);
        my_ctx->sctx.swm_obj = (void*)lammps_swm;
    }
    else if(strcmp(o_params->workload_name, "nekbone") == 0 || strcmp(o_params->workload_name, "nekbone1") == 0)
    {
        NEKBONESWMUserCode * nekbone_swm = new NEKBONESWMUserCode(root, generic_ptrs);
        my_ctx->sctx.swm_obj = (void*)nekbone_swm;
    }
    else if(strcmp(o_params->workload_name, "nearest_neighbor") == 0)
    {
        NearestNeighborSWMUserCode * nn_swm = new NearestNeighborSWMUserCode(root, generic_ptrs);
        my_ctx->sctx.swm_obj = (void*)nn_swm;
    }
    else if(strcmp(o_params->workload_name, "incast") == 0 || strcmp(o_params->workload_name, "incast1") == 0 || strcmp(o_params->workload_name, "incast2") == 0)
    {
        AllToOneSWMUserCode * incast_swm = new AllToOneSWMUserCode(root, generic_ptrs);
        my_ctx->sctx.swm_obj = (void*)incast_swm;
    }
    else if(strcmp(o_params->workload_name, "spread") == 0)
    {
        OneToManySWMUserCode * spread_swm = new OneToManySWMUserCode(root, generic_ptrs);
        my_ctx->sctx.swm_obj = (void*)spread_swm;
    }
    else if(strcmp(o_params->workload_name, "allreduce") == 0 || strcmp(o_params->workload_name, "allreduce32") == 0 || strcmp(o_params->workload_name, "allreduce256") == 0)
    {
        AllReduceSWMUserCode * allreduce_swm = new AllReduceSWMUserCode(root, generic_ptrs);
        my_ctx->sctx.swm_obj = (void*)allreduce_swm;
    }
    else if(strcmp(o_params->workload_name, "many_to_many") == 0 || strcmp(o_params->workload_name, "many_to_many1") == 0)
    {
        ManyToManySWMUserCode * many_to_many_swm = new ManyToManySWMUserCode(root, generic_ptrs);
        my_ctx->sctx.swm_obj = (void*)many_to_many_swm;
    }
    else if(strcmp(o_params->workload_name, "milc") == 0)
    {
        MilcSWMUserCode * milc_swm = new MilcSWMUserCode(root, generic_ptrs);
        my_ctx->sctx.swm_obj = (void*)milc_swm;
    }
    else if(strcmp(o_params->workload_name, "periodic_aggressor") == 0)
    {
        PeriodicAggressor * periodic_aggressor_swm = new PeriodicAggressor(root, generic_ptrs);
        my_ctx->sctx.swm_obj = (void*)periodic_aggressor_swm;
    }

    if(global_prod_thread == NULL)
    {
        ABT_xstream_self(&self_es);
        ABT_thread_self(&global_prod_thread);
    }
    ABT_thread_create_on_xstream(self_es, 
            &workload_caller, (void*)&(my_ctx->sctx),
            ABT_THREAD_ATTR_NULL, &(my_ctx->sctx.producer));

    rank_mpi_compare cmp;
    cmp.app_id = app_id;
    cmp.rank = rank;

    if(!rank_tbl)
    {
        rank_tbl = qhash_init(hash_rank_compare, quickhash_64bit_hash, nprocs);
        if(!rank_tbl)
            return -1;
    }
    qhash_add(rank_tbl, &cmp, &(my_ctx->hash_link));
    rank_tbl_pop++;

    return 0;
}

static void comm_online_workload_get_next(int app_id, int rank, struct codes_workload_op * op)
{
    /* At this point, we will use the "call" function. The send/receive/wait
     * definitions will be replaced by our own function definitions that will do a
     * yield to argobots if an event is not available. */
    /* if shared queue is empty then yield */

    rank_mpi_context * temp_data;
    struct qhash_head * hash_link = NULL;
    rank_mpi_compare cmp;
    cmp.rank = rank;
    cmp.app_id = app_id;
    hash_link = qhash_search(rank_tbl, &cmp);
    if(!hash_link)
    {
        printf("\n not found for rank id %d , %d", rank, app_id);
        op->op_type = CODES_WK_END;
        return;
    }
    temp_data = qhash_entry(hash_link, rank_mpi_context, hash_link);
    assert(temp_data);
    while(temp_data->sctx.fifo.empty())
    {
        ABT_thread_yield_to(temp_data->sctx.producer); 
    }
    struct codes_workload_op * front_op = temp_data->sctx.fifo.front();
    assert(front_op);
    *op = *front_op;
    temp_data->sctx.fifo.pop_front();
    return;
}
static int comm_online_workload_get_rank_cnt(const char *params, int app_id)
{
    online_comm_params * o_params = (online_comm_params*)params;
    int nprocs = o_params->nprocs;
    return nprocs;
}

static int comm_online_workload_finalize(const char* params, int app_id, int rank)
{
    rank_mpi_context * temp_data;
    struct qhash_head * hash_link = NULL;
    rank_mpi_compare cmp;
    cmp.rank = rank;
    cmp.app_id = app_id;
    hash_link = qhash_search(rank_tbl, &cmp);
    if(!hash_link)
    {
        printf("\n not found for rank id %d , %d", rank, app_id);
        return -1;
    }
    temp_data = qhash_entry(hash_link, rank_mpi_context, hash_link);
    assert(temp_data);

    ABT_thread_join(temp_data->sctx.producer);    
    ABT_thread_free(&(temp_data->sctx.producer));
    return 0;
}
extern "C" {
/* workload method name and function pointers for the CODES workload API */
struct codes_workload_method online_comm_workload_method =
{
    //.method_name =
    (char*)"online_comm_workload",
    //.codes_workload_read_config = 
    NULL,
    //.codes_workload_load = 
    comm_online_workload_load,
    //.codes_workload_get_next = 
    comm_online_workload_get_next,
    // .codes_workload_get_next_rc2 = 
    NULL,
    // .codes_workload_get_rank_cnt
    comm_online_workload_get_rank_cnt,
    // .codes_workload_finalize = 
    comm_online_workload_finalize
};
} // closing brace for extern "C"

