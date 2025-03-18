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
#include "union_util.h"

//#ifdef USE_SWM
#include "lammps.h"
#include "nekbone_swm_user_code.h"
#include "nearest_neighbor_swm_user_code.h"
#include "all_to_one_swm_user_code.h"
#include "milc_swm_user_code.h"
#include "abt.h"
//#endif

#define ALLREDUCE_SHORT_MSG_SIZE 2048

#define DBG_COMM 0
#define DBG_LINKING 0
#define DBG_TMP 0
#define CHECKPOINT_HASH_TABLE_SIZE 251
#define DEFAULT_WR_BUF_SIZE (16 * 1024 * 1024)   /* 16 MiB default */

#define THISMIN(a,b) ((a) < (b)) ? (a) : (b)

using namespace std;

static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;
static int total_rank_cnt = 0;
static ABT_thread global_prod_thread = NULL;
static ABT_xstream self_es;
static long cpu_freq = 1.0;
static long num_allreduce = 0;
static long num_isends = 0;
static long num_irecvs = 0;
static long num_barriers = 0;
static long num_sends = 0;
static long num_recvs = 0;
static long num_sendrecv = 0;
static long num_waitalls = 0;

//static std::map<int64_t, int> send_count;
//static std::map<int64_t, int> isend_count;
//static std::map<int64_t, int> allreduce_count;

struct shared_context {
    int my_rank;
    uint32_t wait_id;
    int num_ranks;
    char workload_name[MAX_NAME_LENGTH_WKLD];
    void * swm_obj;
    void * conc_params;
    bool isconc;
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
void UNION_MPI_Comm_size (UNION_Comm comm, int *size) 
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
    // printf("ranks %d\n", sctx->num_ranks);
}

void UNION_MPI_Comm_rank( UNION_Comm comm, int *rank ) 
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

void UNION_MPI_Finalize()
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

    if(DBG_COMM){
        printf("\nUNION FINALIZE src %d ", sctx->my_rank);
        printf("\nnum_sends %ld num_recvs %ld num_isends %ld num_irecvs %ld num_allreduce %ld num_barrier %ld num_waitalls %ld\n", 
                num_sends, num_recvs, num_isends, num_irecvs, num_allreduce, num_barriers, num_waitalls);
        // printf("Rank %d yield to CODES thread: %p\n", sctx->my_rank, global_prod_thread);
    }

    ABT_thread_yield_to(global_prod_thread);
}

void UNION_Compute(long cycle_count)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_DELAY;
    wrkld_per_rank.u.delay.nsecs = cycle_count;
    wrkld_per_rank.u.delay.seconds = (cycle_count) / (1000.0 * 1000.0 * 1000.0);
    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&wrkld_per_rank);
    if(DBG_COMM){
        printf("\nUNION COMPUTE src %d: %ld ns ", sctx->my_rank, cycle_count);
    }
    ABT_thread_yield_to(global_prod_thread);
}

void UNION_Mark_Iteration(UNION_TAG iter_tag)
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

    if(DBG_COMM){
        printf("\nUNION MARKITERATION src %d ", sctx->my_rank);
    }

    ABT_thread_yield_to(global_prod_thread);
}


void UNION_IO_OPEN_FILE(int fid)
{
    struct codes_workload_op op;
    op.op_type = CODES_WK_OPEN;
    op.u.open.file_id = fid;
    op.u.open.create_flag = 1;

    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&op);

    if(DBG_TMP){
        printf("\nUNION IO OPEN src %d ", sctx->my_rank);
    }

    ABT_thread_yield_to(global_prod_thread);

}

void UNION_IO_WRITE(int fid, long size)
{
    struct codes_workload_op op;
    op.op_type = CODES_WK_WRITE;
    op.u.write.file_id = fid;
    op.u.write.offset = 0;
    op.u.write.size = size;

    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&op);
    
    if(DBG_TMP){
        printf("\nUNION IO WRITE src %d ", sctx->my_rank);
    }

    ABT_thread_yield_to(global_prod_thread);
}

void UNION_IO_READ(int fid, long size)
{
    struct codes_workload_op op;
    op.op_type = CODES_WK_READ;
    op.u.read.file_id = fid;
    op.u.read.offset = 0;
    op.u.read.size = size;

    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&op);
    
    if(DBG_TMP){
        printf("\nUNION IO READ src %d ", sctx->my_rank);
    }

    ABT_thread_yield_to(global_prod_thread);
}

void UNION_IO_CLOSE_FILE(int fid)
{
    struct codes_workload_op op;
    op.op_type = CODES_WK_CLOSE;
    op.u.close.file_id = fid;

    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&op);
    
    if(DBG_TMP){
        printf("\nUNION IO READ src %d ", sctx->my_rank);
    }

    ABT_thread_yield_to(global_prod_thread);    
}

void UNION_MPI_Send(const void *buf, 
            int count, 
            UNION_Datatype datatype, 
            int dest, 
            int tag,
            UNION_Comm comm)
{
    /* add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    int datatypesize;
    UNION_Type_size(datatype, &datatypesize);

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
    if(DBG_TMP){
        printf("\nUNION SEND src %d dst %d: %lld bytes ", sctx->my_rank, dest,
                wrkld_per_rank.u.send.num_bytes);
    // printf("Rank %d yield to CODES thread: %p\n", sctx->my_rank, global_prod_thread);
    }
    int rc = ABT_thread_yield_to(global_prod_thread);
    num_sends++;    
}

void UNION_MPI_Recv(void *buf, 
            int count, 
            UNION_Datatype datatype, 
            int source, 
            int tag,
            UNION_Comm comm, 
            UNION_Status *status)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    int datatypesize;
    UNION_Type_size(datatype, &datatypesize);

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
    if(DBG_COMM){
        printf("\nUNION RECV src %d dst %d: %lld bytes ", source, sctx->my_rank, 
            wrkld_per_rank.u.recv.num_bytes);
    // printf("Rank %d yield to CODES thread: %p\n", sctx->my_rank, global_prod_thread);
    }

    ABT_thread_yield_to(global_prod_thread);
    num_recvs++;    
}

void UNION_MPI_Sendrecv(const void *sendbuf, 
            int sendcount, 
            UNION_Datatype sendtype,
            int dest, 
            int sendtag,
            void *recvbuf, 
            int recvcount, 
            UNION_Datatype recvtype,
            int source, 
            int recvtag,
            UNION_Comm comm, 
            UNION_Status *status)
{
    /* sendrecv events */
    struct codes_workload_op send_op;

    int datatypesize1, datatypesize2;
    UNION_Type_size(sendtype, &datatypesize1);
    UNION_Type_size(recvtype, &datatypesize2);

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
    if(DBG_COMM){
        printf("\nUNION SENDRECV ssrc %d sdst %d: %lld bytes; rsrc %d rdst %d: %lld bytes ", sctx->my_rank, dest,
                send_op.u.send.num_bytes, source, sctx->my_rank, recv_op.u.recv.num_bytes);
    }
    ABT_thread_yield_to(global_prod_thread);
    num_sendrecv++;
}


void UNION_MPI_Barrier(UNION_Comm comm)
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

        UNION_MPI_Sendrecv(NULL, 0, UNION_Int, dest, 1234, NULL, 0, UNION_Int, src, 1234,
                comm, NULL);

        mask <<= 1;
    }
    num_barriers++; 
    // if(DBG_COMM){
    //     printf("UNION BARRIER src %d\n", sctx->my_rank);
    // }
}

void UNION_MPI_Isend(const void *buf, 
            int count, 
            UNION_Datatype datatype, 
            int dest, 
            int tag,
            UNION_Comm comm, 
            UNION_Request *request)
{
    /* add an event in the shared queue and then yield */
    //    printf("\n Sending to rank %d ", comm_id);
    struct codes_workload_op wrkld_per_rank;

    int datatypesize;
    UNION_Type_size(datatype, &datatypesize);

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
    if(DBG_COMM){
        printf("\nUNION ISEND src %d dst %d: %lld bytes ", sctx->my_rank, dest,
                wrkld_per_rank.u.send.num_bytes);
    }

    ABT_thread_yield_to(global_prod_thread);
    num_isends++;
}

void UNION_MPI_Irecv(void *buf, 
            int count, 
            UNION_Datatype datatype, 
            int source, 
            int tag,
            UNION_Comm comm, 
            UNION_Request *request)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    int datatypesize;
    UNION_Type_size(datatype, &datatypesize);

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
    if(DBG_COMM){
        printf("\nUNION IRECV src %d dst %d: %lld bytes ", source, sctx->my_rank, 
                wrkld_per_rank.u.recv.num_bytes);    
    }
    ABT_thread_yield_to(global_prod_thread);
    num_irecvs++;    
}

void UNION_MPI_Wait(UNION_Request *request,
        UNION_Status *status)
{
    /* Add an event in the shared queue and then yield */
    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_WAIT;
    wrkld_per_rank.u.wait.req_id = *(UNION_Request *)request;   

    /* Retreive the shared context state */
    ABT_thread prod;
    void * arg;
    int err = ABT_thread_self(&prod);
    assert(err == ABT_SUCCESS);
    err =  ABT_thread_get_arg(prod, &arg);
    assert(err == ABT_SUCCESS);
    struct shared_context * sctx = static_cast<shared_context*>(arg);
    sctx->fifo.push_back(&wrkld_per_rank);
    if(DBG_COMM){
        printf("\nUNION WAIT src %d ",sctx->my_rank);    
    }
    ABT_thread_yield_to(global_prod_thread);       
}

void UNION_MPI_Waitall(int count, 
            UNION_Request array_of_requests[], 
            UNION_Status array_of_statuses[])
{
    num_waitalls++;
    for(int i = 0; i < count; i++)
        UNION_MPI_Wait(&array_of_requests[i], UNION_STATUSES_IGNORE);

    // if(DBG_COMM){
    //     printf("UNION WAITALL count %d\n", count);    
    // }  
}

void UNION_MPI_Reduce(const void *sendbuf, 
            void *recvbuf, 
            int count, 
            UNION_Datatype datatype,
            UNION_Op op, 
            int root, 
            UNION_Comm comm)
{
    //todo
}

void UNION_MPI_Allreduce(const void *sendbuf, 
            void *recvbuf, 
            int count, 
            UNION_Datatype datatype,
            UNION_Op op, 
            UNION_Comm comm)
{
    int comm_size, rank, type_size, i, send_idx, recv_idx, last_idx, send_cnt, recv_cnt;
    int pof2, mask, rem, newrank, newdst, dst, *cnts, *disps;

    UNION_MPI_Comm_size(comm, &comm_size);
    UNION_MPI_Comm_rank(comm, &rank);
    UNION_Type_size(datatype, &type_size);

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
            UNION_MPI_Send(NULL, count, datatype, rank+1, -1002, comm);
            newrank = -1;
        } else { /* odd */
            UNION_MPI_Recv(NULL, count, datatype, rank-1, -1002, comm, NULL);
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

                UNION_MPI_Sendrecv(NULL, count, datatype, dst, -1002, NULL, count, datatype, dst, -1002, comm, NULL);
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

                UNION_MPI_Sendrecv(NULL, send_cnt, datatype, dst, -1002, NULL, recv_cnt, datatype, dst, -1002, comm, NULL);

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

                UNION_MPI_Sendrecv(NULL, send_cnt, datatype, dst, -1002, NULL, recv_cnt, datatype, dst, -1002, comm, NULL);

                if (newrank > newdst) send_idx = recv_idx;
                mask >>= 1;
            }
        }
    } 

    if(rank < 2*rem) {
        if(rank % 2) {/* odd */
            UNION_MPI_Send(NULL, count, datatype, rank-1, -1002, comm);
        } else {
            UNION_MPI_Recv(NULL, count, datatype, rank+1, -1002, comm, NULL);
        }
    }

    if(cnts) free(cnts);
    if(disps) free(disps);    
}


void bcast_binomial(void *buffer,
              int rank,
              int count,
              UNION_Datatype datatype,
              int root,
              UNION_Comm comm)
{
  int comm_size, src, dst, relative_rank, mask;
  UNION_Status status;
  UNION_MPI_Comm_size(comm, &comm_size);

  relative_rank = (rank >= root) ? rank - root : rank - root + comm_size;

  mask = 0x1;
  while(mask < comm_size)
  {
    if(relative_rank & mask)
    {
      src = rank - mask;
      if(src < 0) src += comm_size;
      UNION_MPI_Recv(buffer,count,datatype,src,-1005,comm, &status);
      break;
    }
    mask <<= 1;
  }

  mask >>=1;
  while(mask > 0)
  {
    if(relative_rank + mask < comm_size)
    {
      dst = rank + mask;
      if(dst >= comm_size) dst -= comm_size;
      UNION_MPI_Send(buffer,count,datatype,dst,-1005,comm);
    }
    mask >>= 1;
  }
}

void bcast_scatter_doubling_allgather(void *buffer,
              int rank,
              int count,
              UNION_Datatype datatype,
              int root,
              UNION_Comm comm)
{
  int comm_size, dst, relative_rank, mask, scatter_size, curr_size, recvcount, recv_size = 0;
  UNION_Status status;
  int j, k, i, tmp_mask;
  int type_size, nbytes = 0;
  int relative_dst, dst_tree_root, my_tree_root, send_offset, recv_offset;

  UNION_Type_size(datatype, &type_size);
  UNION_MPI_Comm_size(comm, &comm_size);

  relative_rank = (rank >= root) ? rank - root : rank - root + comm_size;
  
  if(comm_size == 1) return;

  nbytes = type_size * count;
  if(nbytes == 0) return;

  scatter_size = (nbytes + comm_size - 1)/comm_size; /* ceiling division */
  curr_size = THISMIN(scatter_size, (nbytes - (relative_rank * scatter_size)));

  if (curr_size < 0) curr_size = 0;

  mask = 0x1;
  i = 0;

  while(mask < comm_size) {
    relative_dst = relative_rank ^ mask;
    dst = (relative_dst + root) % comm_size;

    dst_tree_root = relative_dst >> i;
    dst_tree_root <<= i;

    my_tree_root = relative_rank >> i;
    my_tree_root <<= i;
  
    send_offset = my_tree_root * scatter_size;
    recv_offset = dst_tree_root * scatter_size;

    if(relative_dst < comm_size)
    {
      recvcount = (nbytes-recv_offset < 0 ? 0 : nbytes-recv_offset);
      UNION_MPI_Sendrecv(buffer,curr_size,UNION_Byte,dst,-1005,buffer,recvcount,UNION_Byte,dst,-1005,comm,&status);
      curr_size += recv_size;
    }

    mask <<= 1;
    i++;
  }
}

void bcast_scatter_ring_allgather(void *buffer,
              int rank,
              int count,
              UNION_Datatype datatype,
              int root,
              UNION_Comm comm)
{
  int comm_size, scatter_size, j, i, nbytes, type_size;
  int left, right, jnext, curr_size = 0;
  int recvd_size;
  UNION_Status status;

  UNION_Type_size(datatype, &type_size);
  UNION_MPI_Comm_size(comm, &comm_size);

  if(comm_size == 1) return;

  nbytes = type_size * count;
  if (nbytes == 0) return;

  scatter_size = (nbytes + comm_size - 1)/comm_size; /* ceiling division */

  curr_size = THISMIN(scatter_size,  nbytes - ((rank - root + comm_size) % comm_size) * scatter_size);
  if(curr_size < 0) curr_size = 0;

  left  = (comm_size + rank - 1) % comm_size;
  right = (rank + 1) % comm_size;
  j = rank;
  jnext = left;

  for (i=1; i<comm_size; i++)
  {
    int left_count, right_count, left_disp, right_disp, rel_j, rel_jnext;
    rel_j     = (j     - root + comm_size) % comm_size;
    rel_jnext = (jnext - root + comm_size) % comm_size;
    left_count = THISMIN(scatter_size, (nbytes - rel_jnext * scatter_size));
    if(left_count < 0) left_count = 0;
    left_disp = rel_jnext * scatter_size;
    right_count = THISMIN(scatter_size, (nbytes - rel_j * scatter_size));
    if(right_count < 0) right_count = 0;
    right_disp = rel_j * scatter_size;

    UNION_MPI_Sendrecv(buffer,right_count,UNION_Byte,right,-1005,buffer,left_count,UNION_Byte,left,-1005,comm,&status);  
    curr_size += recvd_size;
    j = jnext;
    jnext = (comm_size + jnext - 1) % comm_size;
  }
}


void UNION_MPI_Bcast(void *buffer, 
            int count, 
            UNION_Datatype datatype, 
            int root, 
            UNION_Comm comm)
{
    int type_size, comm_size, rank;
    UNION_Type_size(datatype, &type_size);
    UNION_MPI_Comm_rank(UNION_Comm_World, &rank);
    UNION_MPI_Comm_size(UNION_Comm_World, &comm_size);
    int nbytes = count * type_size;

    if((nbytes < 12288) || (comm_size < 8)) {
    //use binomial algorithm
    bcast_binomial(buffer,rank,count,datatype,root,comm);
    } else if((nbytes < 524288) && !(comm_size & (comm_size - 1))) {
    //use scatter followed by recursive doubling allgather
    bcast_scatter_doubling_allgather(buffer,rank,count,datatype,root,comm);
    } else {
    //use scatter followed by ring allgather
    bcast_scatter_ring_allgather(buffer,rank,count,datatype,root,comm);
    }
    // if(DBG_COMM){
    //     printf("BCAST src %d\n", root);    
    // }  
}

void UNION_MPI_Alltoallv(const void *sendbuf, 
            const int *sendcounts, 
            const int *sdispls,
            UNION_Datatype sendtype, 
            void *recvbuf, 
            const int *recvcounts,
            const int *rdispls, 
            UNION_Datatype recvtype, 
            UNION_Comm comm)
{
    int comm_size, i, j;
    int dst, rank, req_cnt, req_num = 1;
    int ii, ss, bblock;
    int type_size;

    bblock = 32; //equivalent of MPIR_CVAR_ALLTOALL_THROTTLE in Mpich

    UNION_Status starray[2*bblock];
    UNION_Request reqarray[2*bblock];

    UNION_MPI_Comm_size(comm, &comm_size);
    UNION_MPI_Comm_rank(comm, &rank);


    for(ii=0; ii<comm_size; ii+=bblock) {

        req_cnt = 0;
        ss = comm_size-ii < bblock ? comm_size-ii : bblock;

        for ( i=0; i<ss; i++ ) {
            dst = (rank+i+ii) % comm_size;
            if (recvcounts[dst]) {
                req_num++; // hopefuly the program is not doing other requests at the same time...
                reqarray[req_cnt] = req_num;
                UNION_MPI_Irecv(NULL, recvcounts[dst], recvtype, dst, -1003, comm, &req_num);
                req_cnt++;
            }
        }

        for ( i=0; i<ss; i++ ) {
            dst = (rank-i-ii+comm_size) % comm_size;
            if (sendcounts[dst]) {
                req_num++;
                reqarray[req_cnt] = req_num;
                UNION_MPI_Isend(NULL, sendcounts[dst], sendtype, dst, -1003, comm, &req_num);
                req_cnt++;
            }
        }
        // UNION_MPI_Waitall(req_cnt, reqarray, starray);
        UNION_MPI_Barrier(comm);
    } 
}

void UNION_MPI_Alltoall(const void *sendbuf, 
            int sendcount, 
            UNION_Datatype sendtype, 
            void *recvbuf,
            int recvcount, 
            UNION_Datatype recvtype, 
            UNION_Comm comm)
{
    int *sendcounts, *sdispls, *recvcounts, *rdispls;
    int i, comm_size;
    UNION_MPI_Comm_size(comm, &comm_size);

    sendcounts = (int *)malloc( comm_size * sizeof(int) );
    recvcounts = (int *)malloc( comm_size * sizeof(int) );
    rdispls = (int *)malloc( comm_size * sizeof(int) );
    sdispls = (int *)malloc( comm_size * sizeof(int) );
 
    for (i=0; i<comm_size; i++) {
        sendcounts[i] = sendcount;
        recvcounts[i] = recvcount;
        rdispls[i] = i * recvcount;
        sdispls[i] = i * sendcount;
    }
    UNION_MPI_Alltoallv(sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts, rdispls, recvtype, comm);
   
    free( sdispls );
    free( rdispls );
    free( recvcounts );
    free( sendcounts );
}



//#ifdef USE_SWM

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

    if(DBG_COMM){
        printf("\nSWM SEND src %d dst %d: %lld bytes ", sctx->my_rank, peer,
                wrkld_per_rank.u.send.num_bytes);
    // printf("Rank %d yield to CODES thread: %p\n", sctx->my_rank, global_prod_thread);
    }

    ABT_thread_yield_to(global_prod_thread);
    num_sends++;
}

/*
 * @param comm_id: communicator ID (For now, UNION_Comm_World)
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

    if(DBG_COMM){
        printf("\nSWM ISEND src %d dst %d: %lld bytes ", sctx->my_rank, peer,
                wrkld_per_rank.u.send.num_bytes);
    }

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

    if(DBG_COMM){
        printf("\nSWM RECV src %d dst %d: %lld bytes ", peer, sctx->my_rank, 
                wrkld_per_rank.u.recv.num_bytes);    
    }

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

    if(DBG_COMM){
        printf("\nSWM IRECV src %d dst %d: %lld bytes ", peer, sctx->my_rank, 
                wrkld_per_rank.u.recv.num_bytes);    
    }

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
    // printf("\n Compute op delay: %f ", delay_in_ns);
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

    if(DBG_COMM){
        printf("\nSWM COMPUTE src %d: %lld ns ", sctx->my_rank, delay_in_ns);    
    }
    
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

    if(DBG_COMM){
        printf("\nSWM WAIT src %d ",sctx->my_rank);    
    }

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

    if(DBG_COMM){
        printf("\nSWM WAITALL src %d: count %d ",sctx->my_rank, len);    
    }

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
    recv_op.u.recv.num_bytes = 0;

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

    if(DBG_COMM){
        printf("\nSWM SENDRECV ssrc %d sdst %d: %d bytes; rsrc %d rdst %d: %lld bytes ", sctx->my_rank, sendpeer,
                sendbytes, recvpeer, sctx->my_rank, recv_op.u.recv.num_bytes);
    }

    ABT_thread_yield_to(global_prod_thread);
    num_sendrecv++;
}

/* @param count: number of bytes in Allreduce
 * @param respbytes: number of bytes to be sent in response (ignore for our
 * purpose)
 * $params comm_id: communicator ID (UNION_Comm_World for our case)
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
        /*
        auto it = allreduce_count.find(count);
        if(it == allreduce_count.end())
        {
            allreduce_count.insert(std::make_pair(count, 1));
        }
        else
        {
            it->second = it->second + 1;
        }
        */
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

    if(DBG_COMM){
        /*    
        auto it = allreduce_count.begin();
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
        printf("\nSWM FINALIZE src %d ", sctx->my_rank);
        printf("\nnum_sends %ld num_recvs %ld num_isends %ld num_irecvs %ld num_allreduce %ld num_barrier %ld num_waitalls %ld\n", 
                num_sends, num_recvs, num_isends, num_irecvs, num_allreduce, num_barriers, num_waitalls);
    }
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

    if(DBG_COMM){
        printf("\nSWM MARKITERATION src %d ", sctx->my_rank);
    }

    ABT_thread_yield_to(global_prod_thread);
}

//#endif


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

    // printf("\n workload name %s ", sctx->workload_name);
    if(strncmp(sctx->workload_name, "conceptual", 10) == 0)
    {
        union_bench_param * conc_params = static_cast<union_bench_param*> (sctx->conc_params);
        // printf("program: %s\n",conc_params->conc_program);
        // printf("argc: %d\n",conc_params->conc_argc);
        int i;
        for (i=0; i<conc_params->conc_argc; i++){
            conc_params->conc_argv[i] = conc_params->config_in[i];
        }
        // conc_params->argv = &conc_params->conc_argv;
        if(DBG_LINKING)
        {
            printf("\nLoad Union Benchmark: %s: %s", conc_params->conc_program, conc_params->conc_argv[1]);
        }        
        union_conc_bench_load(conc_params->conc_program, 
                        conc_params->conc_argc, 
                        conc_params->conc_argv);
    } else if(strcmp(sctx->workload_name, "lammps") == 0)
    {
        LAMMPS_SWM * lammps_swm = static_cast<LAMMPS_SWM*>(sctx->swm_obj);
        lammps_swm->call();
    }
    else if(strcmp(sctx->workload_name, "nekbone") == 0) 
    {
        NEKBONESWMUserCode * nekbone_swm = static_cast<NEKBONESWMUserCode*>(sctx->swm_obj);
        nekbone_swm->call();
    }
    else if(strcmp(sctx->workload_name, "milc") == 0)
    {
        MilcSWMUserCode * milc_swm = static_cast<MilcSWMUserCode*>(sctx->swm_obj);
        milc_swm->call();
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

    // printf("my_ctx nprocs %d\n", my_ctx->sctx.num_ranks);

    void** generic_ptrs;
    int array_len = 1;
    generic_ptrs = (void**)calloc(array_len,  sizeof(void*));
    generic_ptrs[0] = (void*)&rank;

    strcpy(my_ctx->sctx.workload_name, o_params->workload_name);
    boost::property_tree::ptree root, child;
    string swm_path, conc_path;
    bool isconc=0;

    // printf("workload name: %s\n", o_params->workload_name);
    swm_path.append(SWM_DATAROOTDIR);
    if(strcmp(o_params->workload_name, "lammps") == 0)
    {
        swm_path.append("/lammps_workload.json");
    }
    else if(strcmp(o_params->workload_name, "nekbone") == 0)
    {
        swm_path.append("/workload.json"); 
    }
    else if(strcmp(o_params->workload_name, "milc") == 0)
    {
        swm_path.append("/milc_skeleton.json");
    }
    else if(strcmp(o_params->workload_name, "nearest_neighbor") == 0)
    {
        swm_path.append("/skeleton.json"); 
    }
    else if(strcmp(o_params->workload_name, "incast") == 0)
    {
        swm_path.append("/incast.json"); 
    }
    else if(strcmp(o_params->workload_name, "incast1") == 0)
    {
        swm_path.append("/incast1.json"); 
    }
    else if(strcmp(o_params->workload_name, "incast2") == 0)
    {
        swm_path.append("/incast2.json"); 
    }    
    else if(strncmp(o_params->workload_name, "conceptual", 10) == 0)
    {
        conc_path.append(UNION_DATADIR);
        conc_path.append("/conceptual.json");
        isconc = 1;
    }
    else
        tw_error(TW_LOC, "\n Undefined workload type %s ", o_params->workload_name);

    // printf("\nUnion jason path %s\n", conc_path.c_str());
    if(isconc){
        try {
            std::ifstream jsonFile(conc_path.c_str());
            boost::property_tree::json_parser::read_json(jsonFile, root);

            // printf("workload_name: %s\n", o_params->workload_name);
            union_bench_param *tmp_params = (union_bench_param *) calloc(1, sizeof(union_bench_param));
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
            my_ctx->sctx.isconc = 1;
        }
        catch(std::exception & e)
        {
            printf("%s \n", e.what());
            return -1;
        }
    }
    else {
        try {
            std::ifstream jsonFile(swm_path.c_str());
            boost::property_tree::json_parser::read_json(jsonFile, root);
            uint32_t process_cnt = root.get<uint32_t>("jobs.size", 1);
            cpu_freq = root.get<double>("jobs.cfg.cpu_freq") / 1e9; 
        }
        catch(std::exception & e)
        {
            printf("%s \n", e.what());
            return -1;
        }
        my_ctx->sctx.isconc = 0;
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
        else if(strcmp(o_params->workload_name, "milc") == 0)
        {   
            MilcSWMUserCode * milc_swm = new MilcSWMUserCode(root, generic_ptrs);
            my_ctx->sctx.swm_obj = (void*)milc_swm;
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
    }

    if(global_prod_thread == NULL)
    {
        ABT_xstream_self(&self_es);
        ABT_thread_self(&global_prod_thread);
    }
    int rcode = ABT_thread_create_on_xstream(self_es, 
            &workload_caller, (void*)&(my_ctx->sctx),
            ABT_THREAD_ATTR_NULL, &(my_ctx->sctx.producer));

    if(DBG_LINKING)
    {
        printf("\nRank %d create app thread? %d", rank, rcode);
    }
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
        if(DBG_COMM){
            // void * arg;
            // int err =  ABT_thread_get_arg(temp_data->sctx.producer, &arg);
            // assert(err == ABT_SUCCESS);
            // struct shared_context * sctx = static_cast<shared_context*>(arg);
            printf("\nFIFO que empty, yield to rank %d ", rank);
        }
        int rc = ABT_thread_yield_to(temp_data->sctx.producer); 
    }
    struct codes_workload_op * front_op = temp_data->sctx.fifo.front();
    assert(front_op);
    if(DBG_COMM)
    {
        switch(front_op->op_type)
        {
            case CODES_WK_ISEND: printf("\nFIFO pop operation ISEND src %d ", rank);
            case CODES_WK_SEND: printf("\nFIFO pop operation SEND src %d ", rank);
            case CODES_WK_RECV: printf("\nFIFO pop operation RECV src %d ", rank);
            case CODES_WK_IRECV: printf("\nFIFO pop operation IRECV src %d ", rank);
            case CODES_WK_DELAY: printf("\nFIFO pop operation COMPUTE src %d ", rank);
            case CODES_WK_WAIT: printf("\nFIFO pop operation WAIT src %d ", rank);
            case CODES_WK_WAITALL: printf("\nFIFO pop operation WAITALL src %d ", rank);
        }
    }
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
    // printf("Rank %d: Finalize workload for app %d\n", rank, app_id);
    rank_mpi_context * temp_data;
    struct qhash_head * hash_link = NULL;
    rank_mpi_compare cmp;
    cmp.rank = rank;
    cmp.app_id = app_id;
    hash_link = qhash_search(rank_tbl, &cmp);
    if(!hash_link)
    {
        printf("\n not found for rank id %d , %d ", rank, app_id);
        return -1;
    }
    temp_data = qhash_entry(hash_link, rank_mpi_context, hash_link);
    assert(temp_data);

    int rc;
    rc = ABT_thread_join(temp_data->sctx.producer);    
    // printf("thread terminate rc=%d\n", rc);
    rc = ABT_thread_free(&(temp_data->sctx.producer));
    // printf("thread free rc=%d\n", rc);
    if (temp_data->sctx.isconc){
        // printf("free conceptual params\n");
        free(temp_data->sctx.conc_params);   
    }                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                
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

