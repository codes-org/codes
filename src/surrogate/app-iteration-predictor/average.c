#include "surrogate/app-iteration-predictor/average.h"
#include "codes/codes.h"
#include <assert.h>
#include <limits.h>
#include <math.h>

#define master_printf(str, ...) if (g_tw_mynode == 0) { printf(str, __VA_ARGS__); }

static struct avg_app_config my_config = {0};

struct node_data {
    int app_id;
    double acc_iteration_time;
    double prev_iteration_time;
    int acc_iters;
    int last_iter;
};
static struct node_data * arr_node_data = NULL; // array containing info for all nodes

enum APP_STATUS {
    APP_STATUS_running = 0,
    APP_STATUS_just_completed,       // fully ended in this PE
    APP_STATUS_completed_everywhere, // fully ended on all PEs
};

struct app_data {
    int num_nodes;
    int nodes_with_enough_iters;
    int ending_iteration;  // last iteration the simulation will run (aka, num of iterations)
    int nodes_that_have_ended;
    enum APP_STATUS status;  // use ended to stop accumulating data
    // To be used when called by the model. Set by `prepare_fast_forward_jump`
    struct {
        int jump_at_iter;
        int resume_at_iter;
        double restart_at;
    } pred;
};
static struct app_data * arr_app_data = NULL; // array containing info for all apps
static bool ready_to_skip = false;


static void find_max_iter_per_app(int * save_last_iter);
static inline void mpi_allreduce_int_max(int const * local_data, int * result_data, int count);
static inline void mpi_allreduce_int_sum(int const * local_data, int * result_data, int count);
static inline void mpi_allreduce_double_sum(double const * local_data, double * result_data, int count);
static inline void mpi_allreduce_bool_and(bool const * local_data, bool * result_data, int count);
static inline void init_int_array(int * array, int size, int value);
static inline void init_double_array(double * array, int size, double value);
static inline int app_id_for(int nw_id_in_pe) {
    return arr_node_data[nw_id_in_pe].app_id;
}


static void model_calls_init(tw_lp * lp, int nw_id_in_pe, struct app_iter_node_config * config) {
    assert(arr_node_data);
    if (my_config.num_nodes_in_pe <= nw_id_in_pe) {
        tw_error(TW_LOC, "Node id relative to PE (%d) is larger than the number of nodes %d", nw_id_in_pe, my_config.num_nodes_in_pe);
    }

    // Storing node data info
    arr_node_data[nw_id_in_pe].app_id = config->app_id;
    arr_node_data[nw_id_in_pe].last_iter = INT_MIN;

    // Storing app data info
    arr_app_data[config->app_id].num_nodes++;
    if (arr_app_data[config->app_id].ending_iteration == INT_MIN) {
        arr_app_data[config->app_id].ending_iteration = config->app_ending_iter;
    } else {
        if (arr_app_data[config->app_id].ending_iteration != config->app_ending_iter) {
            tw_error(TW_LOC, "Two different ranks for application %d have differing total iterations they will run (%d != %d)", config->app_id, config->app_ending_iter, arr_app_data[config->app_id].ending_iteration);
        }
    }
}


static void model_calls_feed(tw_lp * lp, int nw_id_in_pe, int iter, double iteration_time) {
    (void) lp;
    assert(my_config.num_nodes_in_pe > (size_t) nw_id_in_pe);
    if (app_id_for(nw_id_in_pe) == -1) {
        tw_error(TW_LOC, "Predictor for node was not initialized! Node ID (on PE) %d", nw_id_in_pe);
    }
    struct node_data * node_data = &arr_node_data[nw_id_in_pe];
    if (node_data->last_iter >= iter) { // we only collect iteration data past the previous `last_iter`
        return;
    }
    if (arr_app_data[node_data->app_id].status != APP_STATUS_running) {
        tw_warning(TW_LOC, "Attempting to feed data to application predictor for an application that has either been marked as completed or not configured");
    }
    node_data->acc_iteration_time += iteration_time - node_data->prev_iteration_time;
    node_data->prev_iteration_time = iteration_time;
    node_data->acc_iters++;
    node_data->last_iter = iter;
    // We've hit the required number of iterations to feed our predictor
    if (node_data->acc_iters == my_config.num_iters_to_collect) {
        arr_app_data[node_data->app_id].nodes_with_enough_iters++;
    }
}


static void model_calls_ended(tw_lp * lp, int nw_id_in_pe, double iteration_time) {
    assert(app_id_for(nw_id_in_pe) != -1);
    struct app_data * app_data = &arr_app_data[app_id_for(nw_id_in_pe)];
    app_data->nodes_that_have_ended++;
    if (app_data->nodes_that_have_ended == app_data->num_nodes) {
        app_data->status = APP_STATUS_just_completed;
    }
}


static struct iteration_pred model_calls_predict(tw_lp * lp, int nw_id_in_pe) {
    assert(my_config.num_nodes_in_pe > (size_t) nw_id_in_pe);
    assert(app_id_for(nw_id_in_pe) != -1);
    struct app_data * app_data = &arr_app_data[app_id_for(nw_id_in_pe)];
    return (struct iteration_pred) {
        .resume_at_iter = app_data->pred.resume_at_iter,
        .restart_at = app_data->pred.restart_at,
    };
}

static void model_calls_predict_rc(tw_lp * lp, int nw_id_in_pe) {}

static void reset_with(bool const * app_just_ended) {
    ready_to_skip = false;

    master_printf("Resetting (average) application predictor at GVT %d time %f\n", g_tw_gvt_done, g_tw_pe->GVT_sig.recv_ts)
    
    int last_iter[my_config.num_apps];
    find_max_iter_per_app(last_iter); // We should start tracking iterations from the next iteration

    for (int i=0; i < my_config.num_nodes_in_pe; i++) {
        struct node_data * node_data = &arr_node_data[i];
        node_data->acc_iters = 0;
        node_data->acc_iteration_time = 0;
        if (node_data->last_iter < arr_app_data[node_data->app_id].pred.resume_at_iter) {
            node_data->last_iter = last_iter[node_data->app_id];
            node_data->prev_iteration_time = arr_app_data[node_data->app_id].pred.restart_at;
        }
    }
    for (int i=0; i < my_config.num_apps; i++) {
        arr_app_data[i].nodes_with_enough_iters = 0;
    }

    // If an app just fully ended (ended on all PEs but hasn't been cleaned) then clean it
    for (int i = 0; i < my_config.num_apps; i++) {
        if (app_just_ended[i]) {
            arr_app_data[i].status = APP_STATUS_completed_everywhere;
        }
    }
}

static bool model_calls_have_we_hit_switch(tw_lp * lp, int nw_id_in_pe, int iteration_id) {
    assert(my_config.num_nodes_in_pe > (size_t) nw_id_in_pe);
    int const app_id = app_id_for(nw_id_in_pe);
    if (ready_to_skip && iteration_id == arr_app_data[app_id].pred.jump_at_iter) {
        return true;
    }
    return false;
}

static inline void post_init_share_ending_iteration(void) {
    // Sharing ending_iteration results across PEs
    int ending_iteration_here[my_config.num_apps];
    for (int i = 0; i < my_config.num_apps; i++) {
        ending_iteration_here[i] = arr_app_data[i].ending_iteration;
    }
    int ending_iteration[my_config.num_apps];
    mpi_allreduce_int_max(ending_iteration_here, ending_iteration, my_config.num_apps);

    // Checking that total iterations are the same across nodes
    for (int i = 0; i < my_config.num_apps; i++) {
        struct app_data * app_data = &arr_app_data[i];
        if (app_data->ending_iteration == INT_MIN) {
            if (ending_iteration[i] == INT_MIN) {
                app_data->status = APP_STATUS_completed_everywhere;
                master_printf("Workload/app %d has not been configured to be tracked by iteration predictor (it might be a synthetic workload)\n", i);
            } else {
                // The application has "completed" in this PE already!
                app_data->status = APP_STATUS_just_completed;
            }
            app_data->ending_iteration = ending_iteration[i];
        } else if (ending_iteration[i] != app_data->ending_iteration) {
            tw_error(TW_LOC, "Two different ranks for application %d (on different PEs) have differing total iterations they will run (%d != %d)", i, ending_iteration[i], app_data->ending_iteration);
        }
    }
}

static inline bool has_any_app_ended(bool * save_app_just_ended) {
    // Checking any application has fully ended, in which case we have to restart collecting data
    bool app_just_ended_here[my_config.num_apps];
    for (int i = 0; i < my_config.num_apps; i++) {
        struct app_data * app_data = &arr_app_data[i];
        app_just_ended_here[i] = app_data->status == APP_STATUS_just_completed;
    }
    mpi_allreduce_bool_and(app_just_ended_here, save_app_just_ended, my_config.num_apps);
    for (int i = 0; i < my_config.num_apps; i++) {
        if (save_app_just_ended[i]) {
            return true;
        }
    }
    return false;
}

static inline bool all_apps_ended(void) {
    for (int i = 0; i < my_config.num_apps; i++) {
        struct app_data * app_data = &arr_app_data[i];
        if (app_data->status != APP_STATUS_completed_everywhere) {
            return false;
        }
    }
    return true;
}


static inline bool has_everyone_accumulated_enough() {
    bool everyone = true;
    for (int i = 0; i < my_config.num_apps; i++) {
        struct app_data * app_data = &arr_app_data[i];
        // ignoring apps that have ended already
        bool const app_in_pe = app_data->num_nodes > 0;
        bool const hasnt_ended = app_data->status != APP_STATUS_completed_everywhere;
        if (app_in_pe && hasnt_ended) {
            everyone &= app_data->nodes_with_enough_iters == app_data->num_nodes;
        }
    }
    return everyone;
}

static bool director_calls_is_predictor_ready(void) {
    static bool post_init_done = false;
    if (!post_init_done) {
        post_init_share_ending_iteration();
        post_init_done = true;
    }
    bool app_just_ended[my_config.num_apps];
    if (has_any_app_ended(app_just_ended)) {
        reset_with(app_just_ended);
        return false;
    }

    if (all_apps_ended()) {
        return false;
    }

    // check that all applications have collected data for enough iterations to jump ahead
    bool const everyone_ready_here = has_everyone_accumulated_enough();
    bool everyone_ready;
    mpi_allreduce_bool_and(&everyone_ready_here, &everyone_ready, 1);
    return everyone_ready;
}


static void director_calls_reset(void) {
    bool app_just_ended[my_config.num_apps];
    has_any_app_ended(app_just_ended);
    reset_with(app_just_ended);
}

static void find_avg_iteration_time(double * save_avg_time) {
    double acc_iter_time_here[my_config.num_apps];
    int acc_iters_here[my_config.num_apps];
    init_double_array(acc_iter_time_here, my_config.num_apps, 0.0);
    init_int_array(acc_iters_here, my_config.num_apps, 0);
    for (int i=0; i < my_config.num_nodes_in_pe; i++) {
        struct node_data * node_data = &arr_node_data[i];
        int const app_id = node_data->app_id;
        acc_iter_time_here[app_id] += node_data->acc_iteration_time;
        acc_iters_here[app_id] += node_data->acc_iters;
    }
    double acc_iter_time[my_config.num_apps];
    mpi_allreduce_double_sum(acc_iter_time_here, acc_iter_time, my_config.num_apps);
    int acc_iters[my_config.num_apps];
    mpi_allreduce_int_sum(acc_iters_here, acc_iters, my_config.num_apps);

    for (int i=0; i < my_config.num_apps; i++) {
        if (acc_iters[i]) {
            save_avg_time[i] = acc_iter_time[i] / acc_iters[i];
        }
    }
}

static inline void mpi_allreduce_int_max(int const * local_data, int * result_data, int count) {
    if(MPI_Allreduce(local_data, result_data, count, MPI_INT, MPI_MAX, MPI_COMM_CODES) != MPI_SUCCESS) {
        tw_error(TW_LOC, "MPI_Allreduce failed! Couldn't compute maximum");
    }
}

static inline void mpi_allreduce_int_sum(int const * local_data, int * result_data, int count) {
    if(MPI_Allreduce(local_data, result_data, count, MPI_INT, MPI_SUM, MPI_COMM_CODES) != MPI_SUCCESS) {
        tw_error(TW_LOC, "MPI_Allreduce failed! Couldn't add up");
    }
}

static inline void mpi_allreduce_double_sum(double const * local_data, double * result_data, int count) {
    if(MPI_Allreduce(local_data, result_data, count, MPI_DOUBLE, MPI_SUM, MPI_COMM_CODES) != MPI_SUCCESS) {
        tw_error(TW_LOC, "MPI_Allreduce failed! Couldn't add up");
    }
}

static inline void mpi_allreduce_bool_and(bool const * local_data, bool * result_data, int count) {
    if(MPI_Allreduce(local_data, result_data, count, MPI_C_BOOL, MPI_LAND, MPI_COMM_CODES) != MPI_SUCCESS) {
        tw_error(TW_LOC, "MPI_Allreduce call failed!");
    }
}

static inline void init_int_array(int * array, int size, int value) {
    for (int i = 0; i < size; i++) {
        array[i] = value;
    }
}

static inline void init_double_array(double * array, int size, double value) {
    for (int i = 0; i < size; i++) {
        array[i] = value;
    }
}

static void find_max_iter_per_app(int * save_last_iter) {
    int last_iter_here[my_config.num_apps];
    init_int_array(last_iter_here, my_config.num_apps, INT_MIN);

    for (int i=0; i < my_config.num_nodes_in_pe; i++) {
        struct node_data * node_data = &arr_node_data[i];
        int const app_id = node_data->app_id;
        if (last_iter_here[app_id] < node_data->last_iter) {
            last_iter_here[app_id] = node_data->last_iter;
        }
    }
    mpi_allreduce_int_max(last_iter_here, save_last_iter, my_config.num_apps);
}

static void find_avg_time_for_max_iter(double * save_last_iter_time, int const * last_iter) {
    int acc_iters_here[my_config.num_apps];
    double acc_last_iter_time[my_config.num_apps];
    init_int_array(acc_iters_here, my_config.num_apps, 0);
    init_double_array(acc_last_iter_time, my_config.num_apps, 0.0);
    for (int i=0; i < my_config.num_nodes_in_pe; i++) {
        struct node_data * node_data = &arr_node_data[i];
        int const app_id = node_data->app_id;
        if (node_data->last_iter == last_iter[app_id]) {
            acc_last_iter_time[app_id] += node_data->prev_iteration_time;
            acc_iters_here[app_id]++;
        }
    }
    mpi_allreduce_double_sum(acc_last_iter_time, save_last_iter_time, my_config.num_apps);
    int acc_iters[my_config.num_apps];
    mpi_allreduce_int_sum(acc_iters_here, acc_iters, my_config.num_apps);
    for (int i=0; i < my_config.num_apps; i++) {
        if (acc_iters[i] > 0) {
            save_last_iter_time[i] /= acc_iters[i];
        }
    }
}

static void get_running_apps(bool * is_running) {
    for (int i = 0; i < my_config.num_apps; i++) {
        is_running[i] = arr_app_data[i].status != APP_STATUS_completed_everywhere;
    }
}

static double compute_earliest_end_time(
    bool const * is_running,
    double const * avg_iter_time,
    int const * last_iter,
    double const * last_iter_time) {
    // Compute avg end time for all apps (loop through every node, and add value to avg array)
    double apps_end_time[my_config.num_apps];
    for (int i = 0; i < my_config.num_apps; i++) {
        int const iterations_left = arr_app_data[i].ending_iteration - last_iter[i];
        apps_end_time[i] = last_iter_time[i] + iterations_left * avg_iter_time[i];
    }
    // Pick smallest compute end time/time to skip
    double switch_time = DBL_MAX;
    for (int i = 0; i < my_config.num_apps; i++) {
        if (is_running[i] && switch_time > apps_end_time[i]) {
            switch_time = apps_end_time[i];
        }
    }
    return switch_time;
}

static bool compute_restart_params(
    bool const * is_running,
    double const * avg_iter_time,
    int const * last_iter,
    double const * last_iter_time,
    double switch_time,
    double * apps_restart_at_time,
    int * apps_restart_at_iter) {
    // Find iteration to skip to per node
    bool worth_switching = true;
    for (int i = 0; i < my_config.num_apps; i++) {
        if (!is_running[i]) {
            continue;
        }
        int iters_to_skip = lround((switch_time - last_iter_time[i]) / avg_iter_time[i]);
        apps_restart_at_time[i] = last_iter_time[i] + iters_to_skip * avg_iter_time[i];
        apps_restart_at_iter[i] = last_iter[i] + iters_to_skip;

        // if we are not skipping at least two iterations, there is no point in trying to fastforward
        if (iters_to_skip <= 2) {
            worth_switching = false;
        }
    }
    return worth_switching;
}

static double find_latest_restart_time(bool const * is_running, double const * apps_restart_at_time) {
    // Compute last application to restart (this is restarting_at)
    double last_to_finish = 0;
    for (int i = 0; i < my_config.num_apps; i++) {
        if (is_running[i] && last_to_finish < apps_restart_at_time[i]) {
            last_to_finish = apps_restart_at_time[i];
        }
    }
    return last_to_finish;
}

static void set_app_prediction_data(
    bool const * is_running,
    int const * last_iter,
    int const * apps_restart_at_iter,
    double const * apps_restart_at_time) {
    // Set values for iteration to restart at and iterations to jump for each application
    for (int i = 0; i < my_config.num_apps; i++) {
        if (!is_running[i]) {
            continue;
        }
        arr_app_data[i].pred.jump_at_iter = last_iter[i] + 1;
        arr_app_data[i].pred.resume_at_iter = apps_restart_at_iter[i];
        arr_app_data[i].pred.restart_at = apps_restart_at_time[i];
    }
}

static struct fast_forward_values director_calls_prepare_fast_forward_jump(void) {
    // 0. Check if app is still running
    bool is_running[my_config.num_apps];
    get_running_apps(is_running);

    // 1. Compute end time for each application given current data (pick smallest)
    //   a. Find avg iteration per app
    double avg_iter_time[my_config.num_apps];
    find_avg_iteration_time(avg_iter_time);
    //   b. Find iteration to start switch after
    int last_iter[my_config.num_apps];
    double last_iter_time[my_config.num_apps];
    find_max_iter_per_app(last_iter);
    find_avg_time_for_max_iter(last_iter_time, last_iter);
    //   c. & d. Compute and pick smallest end time/time to skip
    double switch_time = compute_earliest_end_time(is_running, avg_iter_time, last_iter, last_iter_time);

    // 2. Find number of iterations to skip per node given time to skip, then compute when each application is expected to reach this point
    //   a. Find iteration to skip to per node
    double apps_restart_at_time[my_config.num_apps];
    int apps_restart_at_iter[my_config.num_apps];
    bool worth_switching = compute_restart_params(is_running, avg_iter_time, last_iter, last_iter_time, switch_time, apps_restart_at_time, apps_restart_at_iter);

    //   b. Compute last application to restart (this is restarting_at)
    double last_to_finish = find_latest_restart_time(is_running, apps_restart_at_time);

    //   c. If the number of iterations to skip is zero for any app, force reset of predictor tracking
    if (!worth_switching) {
        return (struct fast_forward_values) {
            .status = FAST_FORWARD_restart,
            .restarting_at = last_to_finish,
        };
    }

    // 3. Set values for iteration to restart at and iterations to jump for each application
    set_app_prediction_data(is_running, last_iter, apps_restart_at_iter, apps_restart_at_time);
    ready_to_skip = true;

    return (struct fast_forward_values) {
        .status = FAST_FORWARD_switching,
        .restarting_at = last_to_finish,
    };
}

struct app_iteration_predictor avg_app_iteration_predictor(struct avg_app_config * config_) {
    my_config = *config_;
    arr_node_data = calloc(my_config.num_nodes_in_pe, sizeof(struct node_data));
    arr_app_data = calloc(my_config.num_apps, sizeof(struct app_data));
    for (int i=0; i < my_config.num_nodes_in_pe; i++) {
        struct node_data * node_data = &arr_node_data[i];
        node_data->app_id = -1;
        node_data->last_iter = INT_MIN;
    }
    for (int i=0; i < my_config.num_apps; i++) {
        arr_app_data[i].ending_iteration = INT_MIN;
    }
    return (struct app_iteration_predictor) {
        .model = {
            .init = model_calls_init,
            .feed = model_calls_feed,
            .ended = model_calls_ended,
            .predict = model_calls_predict,
            .predict_rc = model_calls_predict_rc,
            .have_we_hit_switch = model_calls_have_we_hit_switch,
        },
        .director = {
            .reset = director_calls_reset,
            .is_predictor_ready = director_calls_is_predictor_ready,
            .prepare_fast_forward_jump = director_calls_prepare_fast_forward_jump,
        }
    };
}

void free_avg_app_iteration_predictor(void) {
    if (arr_node_data) {
        free(arr_node_data);
    }
    if (arr_app_data) {
        free(arr_app_data);
    }
}
