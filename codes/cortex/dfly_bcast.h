#ifndef DFLY_BCAST_H
#define DFLY_BCAST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	DFLY_BCAST_TREE = 0,
	DFLY_BCAST_LLF,
	DFLY_BCAST_GLF,
        DFLY_BCAST_FOREST
} bcast_type;

typedef void (*send_handler)(int app_id, int rank, int size, int dest, int tag, void* uarg);
typedef void (*recv_handler)(int app_id, int rank, int size, int src,  int tag, void* uarg);

typedef struct {
	int my_rank;
	send_handler do_send;
	recv_handler do_recv;
} comm_handler;

/**
 * Executes a broadcast across the processes of an app.
 * param type   : type of broadcast
 * param app_id : application id
 * param root   : root of the broadcast
 * param nprocs : number of processes in the application
 * param size   : size of the data to broadcast (in bytes)
 * param comm   : comm_handler structure providing send and receive handlers
 * param uarg   : user-provided pointer that will be passed to callbacks
 */
void dfly_bcast(bcast_type type, int app_id, int root, int nprocs, int size, const comm_handler* comm, void* uarg);

#ifdef __cplusplus
}
#endif

#endif
