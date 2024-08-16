#ifndef __DIRECTOR_CLIENT_H_DEFINED__
#define __DIRECTOR_CLIENT_H_DEFINED__

#include <ross.h>
#include "codes/codes_mapping.h"



#define NUM_DIR_TO_NW_EVENT 20


enum SIMULATION_MODE
{
    SIM_MODE_PDES=1,
    SIM_MODE_ITERATION_SURROGATE,
};


typedef struct director_message director_message;
typedef struct director_annotation director_annotation;

enum DIR_EVENTS
{
	DIR_AN_ITER_MARK=1,
    DIR_OP_NW,
    DIR_REGISTERED_EVENT__SWITCH_TO_SURR,
    DIR_REGISTERED_EVENT__SWITCH_TO_PDES,
    DIR_REGISTERED_EVENT__MOVE_TO_NEXT,
};

enum DIR_OPERATIONS //currently unused
{
    DIR_AN_WK_START=1,
    DIR_AN_WK_ITERATION_END,
    DIR_AN_WK_END,
    DIR_OP_SEND,
    DIR_OP_RECV,
};


// director event message struct
struct director_message
{
   int msg_type;
   int op_type;
   int num_rngs;
   int value;
   //model_net_event_return event_rc;
   //struct codes_workload_op * mpi_op;

   void *buffer; // this pointer MUST be at the end of the structure
};

// director annotation struct
struct director_annotation
{
   int an_type;
   int an_value;
};


#ifdef __cplusplus
extern "C"
{
#endif


/**
 * @brief Prepares a request to send to client with the specified command and arguments,
 *        receives a reply

 * @param cmd zmqml request command: 'query', 'launch', execute', send', 'nothing', 'exit'
 * @param args the arguments for launch and execute
 * @param bindata binary data from send
 * @param surrdata containing the 'status' field and optionally 'et' and 'id'. 
 *          'status' is not present, returns a vector with "failed".
 *          Fromat is "<key1>:<val1>;<key2>:<val2>;..."
 * 
 */

//extern char* dir_client_request(const char* cmd, 
//                                const char* args, 
//                                const char* data);


extern void director_lp_register_model(const char *);


/*
extern void director_parse_args(char *args, int **args_array, int *length);
static void director_issue_codes_event(director_state * s, tw_lpid nw_lpid, int dir_registered_event_type, tw_stime ts, tw_lp* lp);
extern void director_register_events(director_state * s, director_message * msg, tw_lp * lp);
extern void dir_test_init(director_state* s, tw_lp* lp);
extern void director_prepare_iteration_dataset(director_state* s, tw_stime * training_data, int training_cycle, int training_records);
extern void director_get_surrogate_prediction(director_state* s, tw_bf * bf, director_message * m, tw_lp * lp, tw_stime* delay_ts);
extern void dir_test_event_handler(director_state* s, tw_bf * bf, director_message * m, tw_lp * lp);
extern void dir_test_finalize(director_state* s, tw_lp* lp);
*/


#ifdef __cplusplus
}
#endif

#endif
