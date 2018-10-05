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
#include <boost/foreach.hpp>
#include "codes/codes-workload.h"
#include "codes/quickhash.h"
#include "codes/codes-jobmap.h"
#include "codes_config.h"
#include "codes/codes-conc-addon.h"

#define ALLREDUCE_SHORT_MSG_SIZE 2048

//#define DBG_COMM 0

using namespace std;

static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;
static int total_rank_cnt = 0;
ABT_thread global_prod_thread = NULL;
ABT_xstream self_es;
long cpu_freq = 1.0;
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
    void * conc_params;
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



/* Conceptual online workload implementations */
void CODES_MPI_Comm_size (MPI_Comm comm, int *size) 
{
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err;

    err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);

    *size = sctx->num_ranks;
}

void CODES_MPI_Comm_rank( MPI_Comm comm, int *rank ) 
{
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err;

    err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);

    *rank = sctx->my_rank;
}

void CODES_MPI_Finalize()
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

    // printf("\n finalize workload for rank %d num_sends %ld num_recvs %ld num_isends %ld num_irecvs %ld num_allreduce %ld num_barrier %ld num_waitalls %ld\n", 
    //         sctx->my_rank, num_sends, num_recvs, num_isends, num_irecvs, num_allreduce, num_barriers, num_waitalls);

    ABT_thread_yield_to(global_prod_thread);
}

void CODES_MPI_Send(const void *buf, 
            int count, 
            MPI_Datatype datatype, 
            int dest, 
            int tag,
            MPI_Comm comm)
{
    /* add an event in the shared queue and then yield */
    //    printf("\n Sending to rank %d ", comm_id);
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_SEND;
    wrkld_per_rank.u.send.tag = tag;
    wrkld_per_rank.u.send.count = count;
    wrkld_per_rank.u.send.data_type = datatype;
    wrkld_per_rank.u.send.num_bytes = count * sizeof(datatype);
    wrkld_per_rank.u.send.dest_rank = dest;

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
    // printf("Rank %d Send Event to dest %d: %d * %lu = %lld, fifo size: %lu\n", sctx->my_rank, dest, count, 
    //         sizeof(datatype), wrkld_per_rank.u.send.num_bytes, sctx->fifo.size());

    // printf("Rank %d yield to CODES thread: %p\n", sctx->my_rank, global_prod_thread);
    int rc = ABT_thread_yield_to(global_prod_thread);
    num_sends++;    
}

void CODES_MPI_Recv(void *buf, 
            int count, 
            MPI_Datatype datatype, 
            int source, 
            int tag,
            MPI_Comm comm, 
            MPI_Status *status)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_RECV;
    wrkld_per_rank.u.recv.tag = tag;
    wrkld_per_rank.u.recv.source_rank = source;
    wrkld_per_rank.u.recv.data_type = datatype;
    wrkld_per_rank.u.recv.count = count;
    wrkld_per_rank.u.recv.num_bytes = count * sizeof(datatype);

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
    // printf("Rank %d Recv event from %d count %d fifo size %lu\n",  sctx->my_rank, source, wrkld_per_rank.u.recv.count, sctx->fifo.size());

    // printf("Rank %d yield to CODES thread: %p\n", sctx->my_rank, global_prod_thread);
    ABT_thread_yield_to(global_prod_thread);
    num_recvs++;    
}

void CODES_MPI_Sendrecv(const void *sendbuf, 
            int sendcount, 
            MPI_Datatype sendtype,
            int dest, 
            int sendtag,
            void *recvbuf, 
            int recvcount, 
            MPI_Datatype recvtype,
            int source, 
            int recvtag,
            MPI_Comm comm, 
            MPI_Status *status)
{
    /* sendrecv events */
    struct codes_workload_op send_op;

    send_op.op_type = CODES_WK_SEND;
    send_op.u.send.tag = sendtag;
    send_op.u.send.count = sendcount;
    send_op.u.send.data_type = sendtype;
    send_op.u.send.num_bytes = sendcount * sizeof(sendtype);
    send_op.u.send.dest_rank = dest;

    struct codes_workload_op recv_op;

    recv_op.op_type = CODES_WK_RECV;
    recv_op.u.recv.tag = recvtag;
    recv_op.u.recv.source_rank = source;
    recv_op.u.recv.count = recvcount;
    recv_op.u.recv.data_type = recvtype;
    recv_op.u.recv.num_bytes = recvcount * sizeof(recvtype);

    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);

    /* Add an event in the shared queue and then yield */
    recv_op.u.recv.dest_rank = sctx->my_rank;
    send_op.u.send.source_rank = sctx->my_rank;
    sctx->fifo.push_back(&send_op);
    sctx->fifo.push_back(&recv_op);

    ABT_thread_yield_to(global_prod_thread);
    num_sendrecv++;
}


void CODES_MPI_Barrier(MPI_Comm comm)
{
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

        CODES_MPI_Sendrecv(NULL, 0, MPI_INT, dest, 1234, NULL, 0, MPI_INT, src, 1234,
                comm, NULL);

        mask <<= 1;
    }
    num_barriers++; 
}

void CODES_MPI_Isend(const void *buf, 
            int count, 
            MPI_Datatype datatype, 
            int dest, 
            int tag,
            MPI_Comm comm, 
            MPI_Request *request)
{
    /* add an event in the shared queue and then yield */
    //    printf("\n Sending to rank %d ", comm_id);
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_ISEND;
    wrkld_per_rank.u.send.tag = tag;    
    wrkld_per_rank.u.send.count = count;
    wrkld_per_rank.u.send.data_type = datatype;
    wrkld_per_rank.u.send.num_bytes = count * sizeof(datatype);
    wrkld_per_rank.u.send.dest_rank = dest;

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

    *request = sctx->wait_id;
    wrkld_per_rank.u.send.req_id = *request;
    sctx->wait_id++;

    ABT_thread_yield_to(global_prod_thread);
    num_isends++;
}

void CODES_MPI_Irecv(void *buf, 
            int count, 
            MPI_Datatype datatype, 
            int source, 
            int tag,
            MPI_Comm comm, 
            MPI_Request *request)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_IRECV;
    wrkld_per_rank.u.recv.tag = tag;
    wrkld_per_rank.u.recv.source_rank = source;
    wrkld_per_rank.u.recv.count = count;
    wrkld_per_rank.u.recv.data_type = datatype;
    wrkld_per_rank.u.recv.num_bytes = count * sizeof(datatype);

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
    
    *request = sctx->wait_id;
    wrkld_per_rank.u.recv.req_id = *request;
    sctx->wait_id++;

    ABT_thread_yield_to(global_prod_thread);
    num_irecvs++;    
}

void CODES_MPI_Waitall(int count, 
            MPI_Request array_of_requests[], 
            MPI_Status array_of_statuses[])
{
    num_waitalls++;
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_WAITALL;
    /* TODO: Check how to convert cycle count into delay? */
    wrkld_per_rank.u.waits.count = count;
    wrkld_per_rank.u.waits.req_ids = (unsigned int*)calloc(count, sizeof(int));    

    for(int i = 0; i < count; i++)
        wrkld_per_rank.u.waits.req_ids[i] = array_of_requests[i];

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

void CODES_MPI_Reduce(const void *sendbuf, 
            void *recvbuf, 
            int count, 
            MPI_Datatype datatype,
            MPI_Op op, 
            int root, 
            MPI_Comm comm)
{
    //todo
}

void CODES_MPI_Allreduce(const void *sendbuf, 
            void *recvbuf, 
            int count, 
            MPI_Datatype datatype,
            MPI_Op op, 
            MPI_Comm comm)
{
    //todo
}

void CODES_MPI_Bcast(void *buffer, 
            int count, 
            MPI_Datatype datatype, 
            int root, 
            MPI_Comm comm)
{
    //todo
}

void CODES_MPI_Alltoall(const void *sendbuf, 
            int sendcount, 
            MPI_Datatype sendtype, 
            void *recvbuf,
            int recvcount, 
            MPI_Datatype recvtype, 
            MPI_Comm comm)
{
    //todo
}

void CODES_MPI_Alltoallv(const void *sendbuf, 
            const int *sendcounts, 
            const int *sdispls,
            MPI_Datatype sendtype, 
            void *recvbuf, 
            const int *recvcounts,
            const int *rdispls, 
            MPI_Datatype recvtype, 
            MPI_Comm comm)
{
    //todo
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

    //printf("\n workload name %s ", sctx->workload_name);
    if(strncmp(sctx->workload_name, "conceptual", 10) == 0)
    {
        conc_bench_param * conc_params = static_cast<conc_bench_param*> (sctx->conc_params);
        // printf("program: %s\n",conc_params->conc_program);
        // printf("argc: %d\n",conc_params->conc_argc);
        int i;
        for (i=0; i<conc_params->conc_argc; i++){
            conc_params->conc_argv[i] = conc_params->config_in[i];
        }
        // conc_params->argv = &conc_params->conc_argv;
        codes_conc_bench_load(conc_params->conc_program, 
                        conc_params->conc_argc, 
                        conc_params->conc_argv);
    }
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

    strcpy(my_ctx->sctx.workload_name, o_params->workload_name);
    boost::property_tree::ptree root, child;
    string swm_path, conc_path;

    if(strncmp(o_params->workload_name, "conceptual", 10) == 0)
    {
        conc_path.append(ONLINE_CONFIGDIR);
        conc_path.append("/conceptual.json");
    }
    else
        tw_error(TW_LOC, "\n Undefined workload type %s ", o_params->workload_name);

    // printf("\npath %s\n", conc_path.c_str());
    try {
        std::ifstream jsonFile(conc_path.c_str());
        boost::property_tree::json_parser::read_json(jsonFile, root);

        // printf("workload_name: %s\n", o_params->workload_name);
        conc_bench_param *tmp_params = (conc_bench_param *) calloc(1, sizeof(conc_bench_param));
        strcpy(tmp_params->conc_program, &o_params->workload_name[11]);
        child = root.get_child(tmp_params->conc_program);
        tmp_params->conc_argc = child.get<int>("argc");
        int i = 0;
        BOOST_FOREACH(boost::property_tree::ptree::value_type &v, child.get_child("argv"))
        {
            assert(v.first.empty()); // array elements have no names
            // tmp_params->conc_argv[i] = (char *) v.second.data().c_str();
            strcpy(tmp_params->config_in[i], v.second.data().c_str());
            i += 1;           
        }
        my_ctx->sctx.conc_params = (void*) tmp_params;
    }
    catch(std::exception & e)
    {
        printf("%s \n", e.what());
        return -1;
    }

    if(global_prod_thread == NULL)
    {
        ABT_xstream_self(&self_es);
        ABT_thread_self(&global_prod_thread);
    }
    ABT_thread_create_on_xstream(self_es, 
            &workload_caller, (void*)&(my_ctx->sctx),
            ABT_THREAD_ATTR_NULL, &(my_ctx->sctx.producer));

    // printf("Rank %d create app thread %p\n", rank, my_ctx->sctx.producer);
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
        // printf("Rank %d fifo empty, yield to app %p\n", rank, temp_data->sctx.producer);
        int rc = ABT_thread_yield_to(temp_data->sctx.producer); 
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

    int rc;
    rc = ABT_thread_join(temp_data->sctx.producer);    
    // printf("thread terminate rc=%d\n", rc);
    rc = ABT_thread_free(&(temp_data->sctx.producer));
    // printf("thread free rc=%d\n", rc);
    free(temp_data->sctx.conc_params);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   
    return 0;
}
extern "C" {
/* workload method name and function pointers for the CODES workload API */
struct codes_workload_method conc_online_comm_workload_method =
{
    //.method_name =
    (char*)"conc_online_comm_workload",
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

