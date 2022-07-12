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

    // printf("\n rank %d finalize workload: num_sends %ld num_recvs %ld num_isends %ld num_irecvs %ld num_allreduce %ld num_barrier %ld num_waitalls %ld\n", 
    //         sctx->my_rank, num_sends, num_recvs, num_isends, num_irecvs, num_allreduce, num_barriers, num_waitalls);
    // printf("Rank %d yield to CODES thread: %p\n", sctx->my_rank, global_prod_thread);
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

    int datatypesize;
    MPI_Type_size(datatype, &datatypesize);

    wrkld_per_rank.op_type = CODES_WK_SEND;
    wrkld_per_rank.u.send.tag = tag;
    wrkld_per_rank.u.send.count = count;
    wrkld_per_rank.u.send.data_type = datatype;
    wrkld_per_rank.u.send.num_bytes = count * datatypesize;
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
    // printf("Rank %d Send Event to dest %d: %lld, fifo size: %lu\n", sctx->my_rank, dest,
    //         wrkld_per_rank.u.send.num_bytes, sctx->fifo.size());

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

    int datatypesize;
    MPI_Type_size(datatype, &datatypesize);

    wrkld_per_rank.op_type = CODES_WK_RECV;
    wrkld_per_rank.u.recv.tag = tag;
    wrkld_per_rank.u.recv.source_rank = source;
    wrkld_per_rank.u.recv.data_type = datatype;
    wrkld_per_rank.u.recv.count = count;
    wrkld_per_rank.u.recv.num_bytes = count * datatypesize;

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
    // printf("Rank %d Recv event from %d bytes %d fifo size %lu\n",  sctx->my_rank, source, wrkld_per_rank.u.recv.num_bytes, sctx->fifo.size());

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

    int datatypesize1, datatypesize2;
    MPI_Type_size(sendtype, &datatypesize1);
    MPI_Type_size(recvtype, &datatypesize2);

    send_op.op_type = CODES_WK_SEND;
    send_op.u.send.tag = sendtag;
    send_op.u.send.count = sendcount;
    send_op.u.send.data_type = sendtype;
    send_op.u.send.num_bytes = sendcount * datatypesize1;
    send_op.u.send.dest_rank = dest;

    struct codes_workload_op recv_op;

    recv_op.op_type = CODES_WK_RECV;
    recv_op.u.recv.tag = recvtag;
    recv_op.u.recv.source_rank = source;
    recv_op.u.recv.count = recvcount;
    recv_op.u.recv.data_type = recvtype;
    recv_op.u.recv.num_bytes = recvcount * datatypesize2;

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

    int datatypesize;
    MPI_Type_size(datatype, &datatypesize);

    wrkld_per_rank.op_type = CODES_WK_ISEND;
    wrkld_per_rank.u.send.tag = tag;    
    wrkld_per_rank.u.send.count = count;
    wrkld_per_rank.u.send.data_type = datatype;
    wrkld_per_rank.u.send.num_bytes = count * datatypesize;
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

    int datatypesize;
    MPI_Type_size(datatype, &datatypesize);

    wrkld_per_rank.op_type = CODES_WK_IRECV;
    wrkld_per_rank.u.recv.tag = tag;
    wrkld_per_rank.u.recv.source_rank = source;
    wrkld_per_rank.u.recv.count = count;
    wrkld_per_rank.u.recv.data_type = datatype;
    wrkld_per_rank.u.recv.num_bytes = count * datatypesize;

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
    int comm_size, rank, type_size, i, send_idx, recv_idx, last_idx, send_cnt, recv_cnt;
    int pof2, mask, rem, newrank, newdst, dst, *cnts, *disps;

    CODES_MPI_Comm_size(comm, &comm_size);
    CODES_MPI_Comm_rank(comm, &rank);
    MPI_Type_size(datatype, &type_size);

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
            CODES_MPI_Send(NULL, count, datatype, rank+1, -1002, comm);
            newrank = -1;
        } else { /* odd */
            CODES_MPI_Recv(NULL, count, datatype, rank-1, -1002, comm, NULL);
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
        if ((count*type_size <= 81920 ) || (count < pof2)) {
            mask = 0x1;
            while (mask < pof2) {
                newdst = newrank ^ mask;
                dst = (newdst < rem) ? newdst*2 + 1 : newdst + rem;

                CODES_MPI_Sendrecv(NULL, count, datatype, dst, -1002, NULL, count, datatype, dst, -1002, comm, NULL);
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

                CODES_MPI_Sendrecv(NULL, send_cnt, datatype, dst, -1002, NULL, recv_cnt, datatype, dst, -1002, comm, NULL);

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

                CODES_MPI_Sendrecv(NULL, send_cnt, datatype, dst, -1002, NULL, recv_cnt, datatype, dst, -1002, comm, NULL);

                if (newrank > newdst) send_idx = recv_idx;
                mask >>= 1;
            }
        }
    } 

    if(rank < 2*rem) {
        if(rank % 2) {/* odd */
            CODES_MPI_Send(NULL, count, datatype, rank-1, -1002, comm);
        } else {
            CODES_MPI_Recv(NULL, count, datatype, rank+1, -1002, comm, NULL);
        }
    }

    if(cnts) free(cnts);
    if(disps) free(disps);    
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
    int *sendcounts, *sdispls, *recvcounts, *rdispls;
    int i, comm_size;

    CODES_MPI_Comm_size(comm, &comm_size);
 
    for (i=0; i<comm_size; i++) {
        sendcounts[i] = sendcount;
        recvcounts[i] = recvcount;
        rdispls[i] = i * recvcount;
        sdispls[i] = i * sendcount;
    }
    CODES_MPI_Alltoallv(sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts, rdispls, recvtype, comm);
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
    int comm_size, i, j;
    int dst, rank, req_cnt, req_num = 1;
    int ii, ss, bblock;
    int type_size;

    bblock = 32; //equivalent of MPIR_CVAR_ALLTOALL_THROTTLE in Mpich

    MPI_Status starray[2*bblock];
    MPI_Request reqarray[2*bblock];

    CODES_MPI_Comm_size(comm, &comm_size);
    CODES_MPI_Comm_rank(comm, &rank);


    for(ii=0; ii<comm_size; ii+=bblock) {

        req_cnt = 0;
        ss = comm_size-ii < bblock ? comm_size-ii : bblock;

        for ( i=0; i<ss; i++ ) {
            dst = (rank+i+ii) % comm_size;
            if (recvcounts[dst]) {
                req_num++; // hopefuly the program is not doing other requests at the same time...
                reqarray[req_cnt] = req_num;
                CODES_MPI_Irecv(NULL, recvcounts[dst], recvtype, dst, -1003, comm, &req_num);
                req_cnt++;
            }
        }

        for ( i=0; i<ss; i++ ) {
            dst = (rank-i-ii+comm_size) % comm_size;
            if (sendcounts[dst]) {
                req_num++;
                reqarray[req_cnt] = req_num;
                CODES_MPI_Isend(NULL, sendcounts[dst], sendtype, dst, -1003, comm, &req_num);
                req_cnt++;
            }
        }
        CODES_MPI_Waitall(req_cnt, reqarray, starray);
    } 
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
    // printf("Pop op %d to CODES\n", front_op->op_type);
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

