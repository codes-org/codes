#include <iostream>
#include <sstream>
#include <vector>
#include <string.h>
#include <stdlib.h>
#include <iterator>

#include <cmath>
#include <chrono>
#include <algorithm> // std::min_element
#include <ios> //std::fixed
#include <iomanip> // std::precision

#include "codes/surrogate/director-client.h"
#include "codes/configuration.h"
#include "zmqmlrequester.h"


#define NUM_ACTIVE_CLIENTS 72 //TODO: this should be calculated at runtime

#define DIR_ZMQ_CMD_LENGTH 64
#define DIR_ZMQ_ARG_LENGTH 100

#define DIR_MAX_PREDICTION 5
#define DIR_MAX_TRAINING_RECORDS 10
/*
 * The Python iteration-time model currently uses history_len=2 and horizon=3,
 * so it needs at least 5 iteration-delta records to train one window.
 * Five deltas require six timestamps in training_data.
 */
#define DIR_MIN_TRAINING_TIMESTAMPS_FOR_FLUSH 6
#define DIR_MAX_DATA_SIZE 15

struct
{
    int surr_iter_start;
    int surr_iter_end;
} director_config_global;

typedef struct director_state director_state;

// Some flag to relocate/clean-up
int evaluate_perf = 1;
int training_enabled = 0;
int director_debug_prints = 0;  //TODO: Move this to the LP state
int director_shutdown_zmqml_server_on_finalize = 0;

std::vector<std::string> director_client_request(const char* cmd, const char* args, const std::string data);
int surrogate_enabled = 0;
int inferencing_enabled = 1;


std::vector<double> total_elapsed_times;
std::vector<double> zmq_processing_times;

// state of the director LP 
struct director_state
{
	tw_lpid director_id;
    int simulation_mode;
    
    int training_cycle_id;
    int training_record_id;
    tw_stime training_data[DIR_MAX_TRAINING_RECORDS];

    /*
     * Commit-only count of training batches durably sent to the ZeroMQ server.
     * This is intentionally not restored by Director RC because it changes
     * only from the commit handler after rollback is impossible.
     */
    int committed_training_cycles;
    //std::vector<tw_stime> training_data_vc;
    
    int next_prediction_index;
    tw_stime predictions[DIR_MAX_PREDICTION];

    void *nw_event_ptr[NUM_DIR_TO_NW_EVENT];
    int nw_event_size[NUM_DIR_TO_NW_EVENT];

    tw_lpid nw_lpid;
};


static void director_free_rc_buffer(director_message *m)
{
    if(m->rc_old_nw_event_buffer != NULL){
        free(m->rc_old_nw_event_buffer);
        m->rc_old_nw_event_buffer = NULL;
    }
}

static void director_save_rc_state(director_state *s, director_message *m)
{
    m->rc_valid = 1;

    m->rc_simulation_mode = s->simulation_mode;
    m->rc_training_cycle_id = s->training_cycle_id;
    m->rc_training_record_id = s->training_record_id;

    for(int i = 0; i < DIR_MAX_TRAINING_RECORDS; i++){
        m->rc_training_data[i] = s->training_data[i];
    }

    m->rc_next_prediction_index = s->next_prediction_index;

    for(int i = 0; i < DIR_MAX_PREDICTION; i++){
        m->rc_predictions[i] = s->predictions[i];
    }

    m->rc_registered_event_type = -1;
    m->rc_old_nw_event_size = 0;
    m->rc_old_nw_event_buffer = NULL;

    m->commit_send_records = 0;
    m->commit_client_id = -1;
    m->commit_training_cycle_id = -1;
    m->commit_num_records = 0;

    for(int i = 0; i < DIR_RC_MAX_TRAINING_RECORDS; i++){
        m->commit_records[i] = 0.0;
    }
}


static void director_send_iteration_records_now(
    int client_id,
    int training_cycle_id,
    const tw_stime *records,
    int num_records
)
{
    if(num_records <= 0){
        return;
    }

    if(num_records > DIR_RC_MAX_TRAINING_RECORDS){
        tw_error(
            TW_LOC,
            "[DIR] Invalid staged training record count: %d",
            num_records
        );
    }

    std::string input_data = "";
    for(int i = 0; i < num_records; i++){
        input_data += std::to_string(records[i]);
        if(i + 1 < num_records){
            input_data += " ";
        }
    }

    char commandstr[DIR_ZMQ_CMD_LENGTH];
    char args[DIR_ZMQ_ARG_LENGTH];

    sprintf(commandstr, "send-records");

    /*
     * Keep the original server-facing argument shape:
     * num_args;client_id;num_records;
     *
     * training_cycle_id is currently used only for local debug output.
     */
    sprintf(args, "%d;%d;%d;", 3, client_id, num_records);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    director_client_request(commandstr, args, input_data);
    clock_gettime(CLOCK_MONOTONIC, &end);

    double latency =
        (end.tv_sec - start.tv_sec) +
        (end.tv_nsec - start.tv_nsec) / 1000000000.0;

    if(director_debug_prints){
        printf(
            "[DIR] committed send-records client=%d cycle=%d num_records=%d latency=%lf\n",
            client_id,
            training_cycle_id,
            num_records,
            latency
        );
    }
}


static void director_restore_rc_state(director_state *s, director_message *m)
{
    if(!m->rc_valid){
        tw_error(TW_LOC, "[DIR RC] Missing RC snapshot for Director event.");
    }

    s->simulation_mode = m->rc_simulation_mode;
    s->training_cycle_id = m->rc_training_cycle_id;
    s->training_record_id = m->rc_training_record_id;

    for(int i = 0; i < DIR_MAX_TRAINING_RECORDS; i++){
        s->training_data[i] = m->rc_training_data[i];
    }

    s->next_prediction_index = m->rc_next_prediction_index;

    for(int i = 0; i < DIR_MAX_PREDICTION; i++){
        s->predictions[i] = m->rc_predictions[i];
    }

    /*
     * Registration events mutate nw_event_size/nw_event_ptr. Restore the
     * previous registered event payload when this event rolls back.
     */
    if(m->rc_registered_event_type >= 0){
        int event_type = m->rc_registered_event_type;

        if(event_type < 0 || event_type >= NUM_DIR_TO_NW_EVENT){
            tw_error(
                TW_LOC,
                "[DIR RC] Invalid registered event type during rollback: %d",
                event_type
            );
        }

        s->nw_event_size[event_type] = m->rc_old_nw_event_size;

        if(m->rc_old_nw_event_size > 0 && m->rc_old_nw_event_buffer != NULL){
            memcpy(
                s->nw_event_ptr[event_type],
                m->rc_old_nw_event_buffer,
                m->rc_old_nw_event_size
            );
        }
    }

    director_free_rc_buffer(m);
    m->rc_valid = 0;
}


std::vector<std::string> director_get_str_list(const char *s, const char delimiter) {
    std::vector<std::string> result;
    std::stringstream ss (s);
    std::string item;

    while (getline (ss, item, delimiter)) { result.push_back (item); }
    return result;
}
std::string director_get_list_str(std::vector<std::string> s, const char delimiter) {
    std::ostringstream mergedstr;
    std::copy(s.begin(), s.end(), std::ostream_iterator<std::string>(mergedstr, " "));
    std::cout << mergedstr.str() << std::endl;
    return mergedstr.str();
}

std::vector<std::string> director_client_request(
    const char* cmd, 
    const char* args,
    const std::string data)
{
    std::vector<std::string> ret;
    /*
    std::cout << cmd << " ARGS " << args << std::endl;
    //if(strcmp(cmd, "send-records") == 0){
        std::cout << data << std::endl;
    //}
    */
    if(strcmp(cmd, "exit") == 0){
         ret = zmqml_request(cmd);
         return ret;
    }

    auto start_time = std::chrono::steady_clock::now(); // TODO - find a way to enclose this in evaluate_perf?

    std::vector<std::string> args_list;
    args_list = director_get_str_list(args, ';');
    ret = zmqml_request(cmd, args_list, data);
    
    if (evaluate_perf == 1){
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration<double>(end_time - start_time).count();

        total_elapsed_times.push_back(duration);
        zmq_processing_times.push_back( std::stod(ret[1]) );

        if(zmq_processing_times.size() == NUM_ACTIVE_CLIENTS){
            double sum = 0;
            for (double ts : zmq_processing_times) sum += ts;
            double mean = sum / zmq_processing_times.size();
            double sum_sq_diff = 0;
            for (double ts : zmq_processing_times) sum_sq_diff += (ts - mean) * (ts - mean);
            double std_dev = sqrt(sum_sq_diff / zmq_processing_times.size());
            auto min = std::min_element(zmq_processing_times.begin(), zmq_processing_times.end());
            auto max = std::max_element(zmq_processing_times.begin(), zmq_processing_times.end());

            zmq_processing_times.clear();

            std::cout << std::setprecision(9) << std::fixed;
            /*
            std::cout << "ZMQ_VALS: ";
            for(auto d: zmq_processing_times)
                std::cout << d << " ;";
            std::cout << std::endl;
            */
            std::cout << "==DIR_STATS zmq-processing: " << cmd
                            << " latency: mean = " << mean 
                            << ", min = " << *min
                            << ", max = " << *max
                            << ", std-deviation = " << std_dev 
                            << std::endl;

            double tsum = 0;
            for (double ts : total_elapsed_times) tsum += ts;
            double tmean = tsum / total_elapsed_times.size();
            double tsum_sq_diff = 0;
            for (double ts : total_elapsed_times) tsum_sq_diff += (ts - tmean) * (ts - tmean);
            double tstd_dev = sqrt(tsum_sq_diff / total_elapsed_times.size());
            auto tmin = std::min_element(total_elapsed_times.begin(), total_elapsed_times.end());
            auto tmax = std::max_element(total_elapsed_times.begin(), total_elapsed_times.end());
            /*
            std::cout << "TOTAL_VALS: ";
            for(auto d: total_elapsed_times)
                std::cout << d << " ;";
            std::cout << std::endl;
            */
            total_elapsed_times.clear();

            std::cout << "==DIR_STATS zmq-total: " << cmd
                            << " latency: mean = " << tmean 
                            << ", min = " << *tmin
                            << ", max = " << *tmax
                            << ", std-deviation = " << tstd_dev 
                            << std::endl;
        }
    }
    /*
    std::cout << cmd << "|" << args << " | ";
    for(auto s: ret)
        std::cout << s << " ;";
    std::cout << std::endl;
    */

    return ret;
}


/* Trigger CODES Event From Director */
static void director_issue_codes_event(director_state * s, tw_lpid nw_lpid, int dir_registered_event_type, tw_stime ts, tw_lp* lp)
{

    tw_event *e;
    void* msg;

    //printf("==DIR: ts: %lf\n", ts);
    e = tw_event_new(nw_lpid, ts, lp);
    msg = (void*)tw_event_data(e);

    memcpy(msg, s->nw_event_ptr[dir_registered_event_type], s->nw_event_size[dir_registered_event_type]);

    //msg->msg_type = dir_registered_event_type;
    tw_event_send(e);
}

void director_register_events(director_state * s, director_message * msg, tw_lp * lp)
{
    int dir_registered_event_type = msg->msg_type;
    int pdes_msg_size = msg->value; 

    if(dir_registered_event_type < 0 || dir_registered_event_type >= NUM_DIR_TO_NW_EVENT){
        tw_error(
            TW_LOC,
            "[DIR] Invalid registered event type: %d",
            dir_registered_event_type
        );
    }

    /*
     * Save the previous registration so reverse computation can restore it
     * if this registration event rolls back.
     */
    msg->rc_registered_event_type = dir_registered_event_type;
    msg->rc_old_nw_event_size = s->nw_event_size[dir_registered_event_type];

    if(msg->rc_old_nw_event_size > 0){
        msg->rc_old_nw_event_buffer = malloc((size_t) msg->rc_old_nw_event_size);
        if(msg->rc_old_nw_event_buffer == NULL){
            tw_error(
                TW_LOC,
                "[DIR RC] Failed to allocate RC registration buffer of size %d",
                msg->rc_old_nw_event_size
            );
        }

        memcpy(
            msg->rc_old_nw_event_buffer,
            s->nw_event_ptr[dir_registered_event_type],
            msg->rc_old_nw_event_size
        );
    }

    //printf("==DIR[%d] DIR Registering dir_event_type:%d (time: %lf)\n", 
    //        s->director_id, dir_registered_event_type, tw_now(lp));

    s->nw_event_size[dir_registered_event_type] = pdes_msg_size;
    memcpy(s->nw_event_ptr[dir_registered_event_type], &msg->buffer, pdes_msg_size);

    //int pdes_event_type = msg->op_type;
    //nw_message *buffer = &msg->buffer;
    //nw_message *saved_msg = s->nw_event_ptr[dir_registered_event_type];
    //printf("==DIR s->director_id: %d | dir_registered_event_type: %d | pdes_event_type: %d (%d)\n",
    //        s->director_id, dir_registered_event_type, pdes_event_type, 
    //        buffer->msg_type);
}




// initializes the director LP 
void director_init(director_state* s, tw_lp* lp)
{
    // initialize the LP's and load the data
    memset(s, 0, sizeof(*s));
    s->simulation_mode = SIM_MODE_PDES;
    s->director_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);

    s->training_cycle_id = 0;
    s->training_record_id = 1;
    s->training_data[0] = tw_now(lp);
    s->committed_training_cycles = 0;
    //s->training_data_vc.push_back(tw_now(lp));
    s->next_prediction_index = -1;
    for(int i = 0; i < DIR_MAX_PREDICTION; i++){
        s->predictions[i] = (tw_stime) 1000000;
    }

    for(int i = 0; i < NUM_DIR_TO_NW_EVENT; i++){
        s->nw_event_ptr[i] = (void*) calloc(1, g_tw_msg_sz);
    }
    
    // get lp_id of the nw that matches this director
    int num_nw_per_mgrp;
    s->nw_lpid;
    num_nw_per_mgrp = codes_mapping_get_lp_count ("MODELNET_GRP", 1, "nw-lp", NULL, 0);
    codes_mapping_get_lp_id("MODELNET_GRP", "nw-lp", NULL, 1, s->director_id / num_nw_per_mgrp, s->director_id % num_nw_per_mgrp, &(s->nw_lpid));

    // Get switching criteria from configuration
    // if we're switch based on iteration - read iter start and end
    // if switch based on virtual time - schedule sending switch event to CODES
    // (stage 2) if switch based on accuracy - schedule polling for accuracy
    // (stage 2) pass training data from CODES to surrogate
    // (stage 3) using workload with network surrogates

    // Update global configs
    if(s->director_id == 1)
    {
        int rc = 1, rc1 = 1, rc2 = 1;
        rc = configuration_get_value_int(&config, "DIRECTOR", "surrogate_enabled", NULL, &surrogate_enabled);
        if(rc)
            surrogate_enabled = 0;
        if(surrogate_enabled){
            rc1 = configuration_get_value_int(&config, "DIRECTOR", "start_iter", NULL, &director_config_global.surr_iter_start);
            rc2 = configuration_get_value_int(&config, "DIRECTOR", "end_iter", NULL, &director_config_global.surr_iter_end);
            if(rc1 || rc2){
                director_config_global.surr_iter_start = 100000; 
                director_config_global.surr_iter_end = 100001; 
                surrogate_enabled = 0;
            }
        }

        rc = configuration_get_value_int(&config, "DIRECTOR", "inferencing_enabled", NULL, &inferencing_enabled);
        if(rc)
            inferencing_enabled = 0;

        rc = configuration_get_value_int(&config, "DIRECTOR", "training_enabled", NULL, &training_enabled);
        if(rc)
            training_enabled = 0;
    
        rc = configuration_get_value_int(&config, "DIRECTOR", "debug_prints", NULL, &director_debug_prints);
        if(rc)
            director_debug_prints = 0;

        rc = configuration_get_value_int(&config, "DIRECTOR", "shutdown_zmqml_server_on_finalize", NULL, &director_shutdown_zmqml_server_on_finalize);
        if(rc)
            director_shutdown_zmqml_server_on_finalize = 0;

        {
            char commandstr[DIR_ZMQ_CMD_LENGTH];
            char args[DIR_ZMQ_ARG_LENGTH];

            sprintf(commandstr, "set-debug");
            sprintf(args, "%d;%d;", 1, director_debug_prints);
            director_client_request(commandstr, args, "");

            if(director_debug_prints){
                printf("[DIR] ZeroMQ server debug prints enabled via DIRECTOR/debug_prints.\n");
            }
        }
}
    //printf("\n==DIR s->director_id: %d | lp->gid: %llu | s->nw_lpid: %llu", s->director_id, LLU(lp->gid), LLU(s->nw_lpid));

    /*
    char commandstr[DIR_ZMQ_CMD_LENGTH];
    char args[DIR_ZMQ_ARG_LENGTH];
    std::vector<std::string> ret_vals;
    
    sprintf(commandstr,"cmd-%d", s->director_id);
    sprintf(args, "2;args-1;");   
    
    ret_vals = director_client_request(commandstr, args, "");
    //printf("UNIVERSE: %s - %s\n", commandstr, ret_vals[0]);
    */

    return;
}

void director_prepare_iteration_dataset(
    director_state* s,
    director_message *m,
    tw_stime * training_data,
    int training_cycle,
    int training_records
)
{
    /*
     * training_records is the number of timestamps.  The Python model consumes
     * deltas between consecutive timestamps, so the payload has
     * training_records - 1 values.
     */
    int num_iteration_records = training_records - 1;

    if(num_iteration_records <= 0){
        return;
    }

    if(num_iteration_records > DIR_RC_MAX_TRAINING_RECORDS){
        tw_error(
            TW_LOC,
            "[DIR] Too many iteration records for commit staging: %d",
            num_iteration_records
        );
    }

    tw_stime staged_records[DIR_RC_MAX_TRAINING_RECORDS];

    for(int i = 0; i < num_iteration_records; i++){
        staged_records[i] = training_data[i + 1] - training_data[i];

        if(staged_records[i] <= 0.0){
            if(director_debug_prints){
                printf(
                    "[DIR] Skipping non-positive iteration record: "
                    "director_id=%llu cycle=%d index=%d value=%lf\n",
                    (unsigned long long) s->director_id,
                    training_cycle,
                    i,
                    staged_records[i]
                );
            }
            return;
        }
    }

    /*
     * In optimistic mode, external ZeroMQ side effects must occur only after
     * the event commits.  In sequential/conservative modes there is no
     * rollback, so keep the immediate behavior.
     */
    if(g_tw_synchronization_protocol == OPTIMISTIC){
        m->commit_send_records = 1;
        m->commit_client_id = (int) s->director_id;
        m->commit_training_cycle_id = training_cycle;
        m->commit_num_records = num_iteration_records;

        for(int i = 0; i < num_iteration_records; i++){
            m->commit_records[i] = staged_records[i];
        }

        if(director_debug_prints){
            printf(
                "[DIR] staged send-records for commit: "
                "director_id=%llu cycle=%d num_records=%d\n",
                (unsigned long long) s->director_id,
                training_cycle,
                num_iteration_records
            );
        }
    }
    else{
        director_send_iteration_records_now(
            (int) s->director_id,
            training_cycle,
            staged_records,
            num_iteration_records
        );
    }
}


static bool director_flush_partial_iteration_dataset_before_inference(director_state* s, director_message *m, tw_lp * lp)
{
    if(!training_enabled){
        return true;
    }

    /*
     * If this Director LP has already sent at least one training batch, the
     * server should already have records for this client. In that case, a
     * too-small current partial buffer is not fatal.
     */
    if(s->training_record_id < DIR_MIN_TRAINING_TIMESTAMPS_FOR_FLUSH){
        if(director_debug_prints){
            printf(
                "[DIR] Not flushing partial training data before inference: "
                "director_id=%llu timestamps=%d min_required=%d "
                "training_cycle_id=%d time=%lf\n",
                (unsigned long long) s->director_id,
                s->training_record_id,
                DIR_MIN_TRAINING_TIMESTAMPS_FOR_FLUSH,
                s->training_cycle_id,
                tw_now(lp)
            );
        }

        return s->training_cycle_id > 0;
    }

    if(director_debug_prints){
        printf(
            "[DIR] Flushing partial training data before inference: "
            "director_id=%llu timestamps=%d deltas=%d time=%lf\n",
            (unsigned long long) s->director_id,
            s->training_record_id,
            s->training_record_id - 1,
            tw_now(lp)
        );
    }

    director_prepare_iteration_dataset(
        s,
        m,
        s->training_data,
        s->training_cycle_id,
        s->training_record_id
    );

    s->training_cycle_id = s->training_cycle_id + 1;
    s->training_record_id = 1;
    s->training_data[0] = tw_now(lp);

    return true;
}


static bool director_iteration_model_ready(director_state *s)
{
    /*
     * Sequential/conservative modes have no rollback hazard, so preserve the
     * previous behavior there.  In optimistic mode, require at least one
     * committed training batch before inference, otherwise the server returns
     * the no-record fallback prediction.
     */
    if(g_tw_synchronization_protocol != OPTIMISTIC){
        return true;
    }

    if(!inferencing_enabled){
        return true;
    }

    return s->committed_training_cycles > 0;
}

void director_get_surrogate_prediction(director_state* s, tw_bf * bf, director_message * m, tw_lp * lp, tw_stime* delay_ts)
{
    // Check if we have sufficient predictions
    if(s->next_prediction_index == -1){ // we need more
        //printf("==DIR[%d] DIR Prediction -- generating set (time: %lf)\n", 
        //    s->director_id, tw_now(lp));
        
        if(inferencing_enabled){
            // Pull more predictions
            std::vector<std::string> ret_vals;
            char commandstr[DIR_ZMQ_CMD_LENGTH];
            char args[DIR_ZMQ_ARG_LENGTH];
            
            sprintf(commandstr, "iteration-time-inference");
            sprintf(args, "%d;%llu;%d;", 3, (unsigned long long) s->director_id, DIR_MAX_PREDICTION); // num-of-args;num-record

            // The Python side primarily uses records previously sent through
            // send-records. Keep the payload empty for now rather than sending
            // hard-coded dummy values.
            std::string input_data = "";
            ret_vals = director_client_request(commandstr, args, input_data);

            if(ret_vals.size() < 3){
                tw_error(
                    TW_LOC,
                    "[DIR] iteration-time-inference returned too few fields: %lu",
                    (unsigned long) ret_vals.size()
                );
            }

            std::vector<std::string> predictions = director_get_str_list(ret_vals[2].c_str(), ' ');

            int i = 0;
            for(auto p: predictions){
                if(p.empty()){
                    continue;
                }

                if(i >= DIR_MAX_PREDICTION){
                    break;
                }

                double pred = std::stod(p);

                if(!std::isfinite(pred) || pred <= 0.0){
                    tw_error(
                        TW_LOC,
                        "[DIR] Invalid iteration-time prediction from ZeroMQ server: %lf",
                        pred
                    );
                }

                s->predictions[i] = (tw_stime) pred;
                i += 1;
            }

            if(i != DIR_MAX_PREDICTION){
                tw_error(
                    TW_LOC,
                    "[DIR] Expected %d iteration-time predictions, received %d",
                    DIR_MAX_PREDICTION,
                    i
                );
            }
        }

        s->next_prediction_index = 0;    
    }
    *delay_ts = s->predictions[s->next_prediction_index];

    s->next_prediction_index = s->next_prediction_index + 1;

    // Check if we've exhuasted the predictions
    if(s->next_prediction_index == DIR_MAX_PREDICTION){
        s->next_prediction_index = -1;
    }
}


void director_event_handler(director_state* s, tw_bf * bf, director_message * m, tw_lp * lp)
{
    director_save_rc_state(s, m);
    
    switch(m->msg_type)
	{
		case DIR_OP_NW:
            if(s->simulation_mode == SIM_MODE_PDES)
            {
                tw_error(TW_LOC, "DIR sent for non-annotation operation during PDES mode.");
            } else if(s->simulation_mode == SIM_MODE_ITERATION_SURROGATE)
            {
                //printf("==DIR[%d] Skipping NW Op type:%d (time: %lf)\n", s->director_id, m->value, tw_now(lp));

                tw_stime delay_ts = 0.001;
                director_issue_codes_event(s, s->nw_lpid, DIR_REGISTERED_EVENT__MOVE_TO_NEXT, delay_ts, lp);
            }
		break;

        case DIR_AN_ITER_MARK:
            //fprintf(iteration_log, "DIR %d (time %lf)\n", s->director_id, tw_now(lp));
            //printf("==DIR[%d] DIR_AN_ITER_MARK m->value: %d (time: %lf)\n", s->director_id, m->value, tw_now(lp));
            if(s->simulation_mode == SIM_MODE_PDES)
            {
                // Manage training data
                if(training_enabled && s->training_record_id < DIR_MAX_TRAINING_RECORDS)
                {// There is space to store more training data
                    s->training_data[s->training_record_id] = tw_now(lp);
                    //s->training_data_vc.push_back(tw_now(lp));
                    s->training_record_id = s->training_record_id + 1;
                }
                if(training_enabled && s->training_record_id == DIR_MAX_TRAINING_RECORDS)
                {// We've filled all training data slots
                    //printf("==DIR[%d] Sending training dataset (time: %lf)\n", s->director_id, tw_now(lp));

                    // Prepare and send training data
                    director_prepare_iteration_dataset(s, m, s->training_data, s->training_cycle_id, DIR_MAX_TRAINING_RECORDS);

                    // Increment cycle counter, reset record counter, and prime dataset
                    s->training_cycle_id = s->training_cycle_id + 1;
                    s->training_record_id = 1;
                    s->training_data[0] = tw_now(lp);
                }
                if(surrogate_enabled
                    && m->value >= director_config_global.surr_iter_start
                    && m->value < director_config_global.surr_iter_end)
                {
                    //printf("==DIR[%d] Triggering switch to SURR (time: %lf)\n", s->director_id, tw_now(lp));

                    /*
                     * Do not enter surrogate mode until this Director LP has
                     * either sent a previous training batch or has enough local
                     * timestamps to flush a trainable partial batch. Otherwise
                     * the first inference call reaches the ZeroMQ server with
                     * records=0/trained=0 and returns only the mechanical
                     * fallback.
                     */
                    if(!director_flush_partial_iteration_dataset_before_inference(s, m, lp)){
                        if(director_debug_prints){
                            printf(
                                "[DIR] Deferring surrogate switch until trainable "
                                "iteration records exist: director_id=%llu "
                                "iter=%d timestamps=%d min_required=%d time=%lf\n",
                                (unsigned long long) s->director_id,
                                m->value,
                                s->training_record_id,
                                DIR_MIN_TRAINING_TIMESTAMPS_FOR_FLUSH,
                                tw_now(lp)
                            );
                        }

                        tw_stime delay_ts = 0.001;
                        director_issue_codes_event(
                            s,
                            s->nw_lpid,
                            DIR_REGISTERED_EVENT__MOVE_TO_NEXT,
                            delay_ts,
                            lp
                        );
                        return;
                    }
                    
                    if(!director_iteration_model_ready(s)){
                        if(director_debug_prints){
                            printf(
                                "[DIR] Deferring surrogate switch until committed "
                                "training is available: director_id=%llu iter=%d "
                                "committed_training_cycles=%d time=%lf\n",
                                (unsigned long long) s->director_id,
                                m->value,
                                s->committed_training_cycles,
                                tw_now(lp)
                            );
                        }

                        tw_stime delay_ts = 0.001;
                        director_issue_codes_event(
                            s,
                            s->nw_lpid,
                            DIR_REGISTERED_EVENT__MOVE_TO_NEXT,
                            delay_ts,
                            lp
                        );
                        return;
                    }

                    s->simulation_mode = SIM_MODE_ITERATION_SURROGATE;
                    tw_stime delay_ts;
                    director_get_surrogate_prediction(s, bf, m, lp, &delay_ts);
                    director_issue_codes_event(s, s->nw_lpid, DIR_REGISTERED_EVENT__SWITCH_TO_SURR, delay_ts, lp);
                    return;
                }
                else
                {
                    tw_stime delay_ts = 0.001;
                    //printf("===[%llu] D-MARK[%llu]: Value=%d\n", s->nw_lpid, lp->gid, m->value );
                    director_issue_codes_event(s, s->nw_lpid, DIR_REGISTERED_EVENT__MOVE_TO_NEXT, delay_ts, lp);
                    return;
                }
            } 
            else if(s->simulation_mode == SIM_MODE_ITERATION_SURROGATE)
            {
                if(m->value == director_config_global.surr_iter_end)
                {
                    //printf("==DIR[%d] Triggering switch to PDES (time: %lf)\n", s->director_id, tw_now(lp));
                
                    s->simulation_mode = SIM_MODE_PDES;
                    tw_stime delay_ts = 0.001;
                    director_issue_codes_event(s, s->nw_lpid, DIR_REGISTERED_EVENT__SWITCH_TO_PDES, delay_ts, lp);

                    if(training_enabled){
                        // Restart training data collection
                        //s->training_data_vc.clear();
                        s->training_data[0] = tw_now(lp);
                        s->training_record_id = 1;
                    }
                    return;
                } 
                else // we need to predict when the next iteration will start
                {
                    tw_stime delay_ts;
                    director_get_surrogate_prediction(s, bf, m, lp, &delay_ts);
                    director_issue_codes_event(s, s->nw_lpid, DIR_REGISTERED_EVENT__MOVE_TO_NEXT, delay_ts, lp);
                    return;
                }
            }
            else
            {
                tw_error(TW_LOC, "[DIR] Simulation mode unknown.");
            }
            
        break;
        
        case DIR_REGISTERED_EVENT__SWITCH_TO_SURR:
        case DIR_REGISTERED_EVENT__SWITCH_TO_PDES:
        case DIR_REGISTERED_EVENT__MOVE_TO_NEXT:
            director_register_events(s, m, lp);
        break;

        default:
        break;
	}
}

void director_event_handler_rc(director_state* s, tw_bf * bf, director_message * m, tw_lp * lp)
{
    director_restore_rc_state(s, m);
}

void director_event_handler_commit(director_state* s, tw_bf * bf, director_message * m, tw_lp * lp)
{
    /*
     * Commit external side effects only after ROSS guarantees this event will
     * not roll back.
     */
    if(m->commit_send_records){
        director_send_iteration_records_now(
            m->commit_client_id,
            m->commit_training_cycle_id,
            m->commit_records,
            m->commit_num_records
        );

        s->committed_training_cycles += 1;

        if(director_debug_prints){
            printf(
                "[DIR] committed training is now available: "
                "director_id=%llu committed_training_cycles=%d\n",
                (unsigned long long) s->director_id,
                s->committed_training_cycles
            );
        }

        m->commit_send_records = 0;
    }

    /*
     * If the event commits without rollback, release any RC-only allocation
     * used to restore a previous registered event payload.
     */
    director_free_rc_buffer(m);
    m->rc_valid = 0;
}

void director_finalize(director_state* s, tw_lp* lp)
{
    /*
     * Do not shut down the external ZeroMQ ML server by default. The server is
     * a reusable service and may be shared across multiple simulation runs.
     * Set DIRECTOR/shutdown_zmqml_server_on_finalize=1 in the config only when this
     * simulation should own and stop the server.
     */

    if (
            s->director_id == 0 &&
            (training_enabled || inferencing_enabled) &&
            director_shutdown_zmqml_server_on_finalize
        ){
            director_client_request("exit", "", "");
        }
    
    //printf("\n==DIR: FINALIZED");
}

tw_lptype dir_lp = {
    (init_f) director_init,
    (pre_run_f) NULL,
    (event_f) director_event_handler,
    (revent_f) director_event_handler_rc,
    (commit_f) director_event_handler_commit,
    (final_f) director_finalize,
    (map_f) codes_mapping,
    sizeof(director_state)
};

extern void director_lp_register_model(const char * dir_lp_name){
    int num_dir_per_mgrp = codes_mapping_get_lp_count ("MODELNET_GRP", 1, "dir-nw-lp", NULL, 0);
    if(num_dir_per_mgrp > 0){
        lp_type_register(dir_lp_name, &dir_lp); // DIRECTOR addition - register type
        //printf("\n==DIR: Registered\n");
    }
}


/*  ==========================================================
    END OF Director Code (To be moved to separate files)
    ==========================================================
*/
