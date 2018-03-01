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
#include "codes/codes-workload.h"
#include "codes/quickhash.h"
#include "codes/codes-jobmap.h"
#include "codes_config.h"
#include "lammps.h"
#include "nekbone_swm_user_code.h"
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#define DBG_COMM 1

using namespace std;

static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;
static int total_rank_cnt = 0;
ABT_thread global_prod_thread = NULL;

struct shared_context {
     int my_rank;
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
    printf("\n send op tag: %d bytes: %d dest: %d ", tag, bytes, peer);
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
    wrkld_per_rank.u.send.req_id = *handle;
    wrkld_per_rank.u.send.num_bytes = bytes;
    wrkld_per_rank.u.send.dest_rank = peer;

#ifdef DBG_COMM
    printf("\n isend op tag: %d req_id: %"PRIu32" bytes: %d dest: %d ", tag, *handle, bytes, peer);
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

#ifdef DBG_COMM
    printf("\n recv op tag: %d source: %d ", tag, peer);
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
    wrkld_per_rank.u.recv.req_id = *handle;
    wrkld_per_rank.u.recv.num_bytes = 0;

#ifdef DBG_COMM
    printf("\n irecv op tag: %d source: %d ", tag, peer);
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

}

void SWM_Compute(long cycle_count)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_DELAY;
    /* TODO: Check how to convert cycle count into delay? */
    wrkld_per_rank.u.delay.nsecs = cycle_count;

#ifdef DBG_COMM
    printf("\n compute op delay: %ld ", cycle_count);
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
    printf("\n wait op req_id: %"PRIu32"\n", req_id);
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
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_WAITALL;
    /* TODO: Check how to convert cycle count into delay? */
    wrkld_per_rank.u.waits.count = len;
    wrkld_per_rank.u.waits.req_ids = (unsigned int*)calloc(len, sizeof(int));    

    for(int i = 0; i < len; i++)
        wrkld_per_rank.u.waits.req_ids[i] = req_ids[i];

#ifdef DBG_COMM
    for(int i = 0; i < len; i++)
        printf("\n wait op req_id: %"PRIu32"\n", req_ids[i]);
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
    
#ifdef DBG_COMM
    printf("\n send/recv op send-tag %d send-bytes %d recv-tag: %d recv-source: %d ", sendtag, sendbytes, recvtag, recvpeer);
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
}

/* @param bytes: number of bytes in Allreduce
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
        SWM_BYTES bytes,
        SWM_BYTES respbytes,
        SWM_COMM_ID comm_id,
        SWM_VC sendreqvc,
        SWM_VC sendrspvc,
        SWM_BUF sendbuf,
        SWM_BUF rcvbuf)
{
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
    printf("\n finalize workload for rank %d ", sctx->my_rank);
#endif
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

    if(strcmp(sctx->workload_name, "lammps") == 0)
    {
        LAMMPS_SWM * lammps_swm = static_cast<LAMMPS_SWM*>(sctx->swm_obj);
        lammps_swm->call();
    }
    else if(strcmp(sctx->workload_name, "nekbone") == 0) 
    {
        NEKBONESWMUserCode * nekbone_swm = static_cast<NEKBONESWMUserCode*>(sctx->swm_obj);
        nekbone_swm->call();
    }
}
static int comm_online_workload_load(const char * params, int app_id, int rank)
{
    /* LOAD parameters from JSON file*/
    online_comm_params * o_params = (online_comm_params*)params;
    int nprocs = o_params->nprocs;
    
    rank_mpi_context *my_ctx;
    my_ctx = (rank_mpi_context*)calloc(1, sizeof(rank_mpi_context));  
    assert(my_ctx); 
    my_ctx->sctx.my_rank = rank; 
    my_ctx->app_id = app_id;

    void** generic_ptrs;
    int array_len = 1;
    generic_ptrs = (void**)calloc(array_len,  sizeof(void*));
    generic_ptrs[0] = (void*)&rank;

    strcpy(my_ctx->sctx.workload_name, o_params->workload_name);
    boost::property_tree::ptree root;
    string path;
    path.append(SWM_DATAROOTDIR);

    if(strcmp(o_params->workload_name, "lammps") == 0)
    {
        path.append("/lammps_workload.json");
    }
    else if(strcmp(o_params->workload_name, "nekbone") == 0)
    {
        path.append("/workload.json"); 
    }
    else
        tw_error(TW_LOC, "\n Undefined workload type %s ", o_params->workload_name);

     printf("\n path %s ", path.c_str());
      try {
            std::ifstream jsonFile(path);
//            root.put("C:.Windows.System", "20 files"); 
//            boost::property_tree::json_parser::write_json("file.json", root);
            boost::property_tree::json_parser::read_json(jsonFile, root);
            uint32_t process_cnt = root.get<uint32_t>("jobs.size", 1);
           }
      catch(std::exception & e)
       {
            printf("%s \n", e.what());
            return -1;
       }
    if(strcmp(o_params->workload_name, "lammps") == 0)
    {
        LAMMPS_SWM * lammps_swm = new LAMMPS_SWM(root, generic_ptrs);
        my_ctx->sctx.swm_obj = (void*)lammps_swm;
    }
    else if(strcmp(o_params->workload_name, "nekbone") == 0)
    {
         NEKBONESWMUserCode * nekbone_swm = new NEKBONESWMUserCode(root, generic_ptrs);
         my_ctx->sctx.swm_obj = (void*)nekbone_swm;
    }
    ABT_xstream self_es;

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

    printf("\n workload created %d %d, table popped ", app_id, rank);
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
        printf("\n Yielding to producer! ");
        ABT_thread_yield_to(temp_data->sctx.producer); 
    }
    struct codes_workload_op * front_op = temp_data->sctx.fifo.front();
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
#ifdef DBG_COMM
    printf("\n finalize workload for rank %d ", rank);
#endif

    ABT_thread_join(temp_data->sctx.producer);    
    ABT_thread_free(&(temp_data->sctx.producer));
    return 0;
}
/* workload method name and function pointers for the CODES workload API */
struct codes_workload_method online_comm_workload_method =
{
    .method_name = (char*)"online_comm_workload",
    .codes_workload_read_config = NULL,
    .codes_workload_load = comm_online_workload_load,
    .codes_workload_get_next = comm_online_workload_get_next,
    .codes_workload_get_next_rc2 = NULL,
    .codes_workload_get_rank_cnt = comm_online_workload_get_rank_cnt,
    .codes_workload_finalize = comm_online_workload_finalize
};


