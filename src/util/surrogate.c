#include <assert.h>
#include <codes/configuration.h>
#include <codes/codes_mapping.h>
#include <codes/surrogate.h>

// Basic level of debugging is 1. It should be always turned on
// because it tells us when a switch to or from surrogate-mode happened.
// It can be deactivated (set to 0) if it ends up being too obnoxious
// Level 0: don't show anything
// Level 1: show when surrogate-mode is activated and deactivated
// Level 2: level 1 and some information at each GVT
// Level 3: level 1 and show extended information at each GVT
#define DEBUG_DIRECTOR 1

// Global variables
int total_terminals = 0;
double ignore_until = 0;

// === Average packet latency functionality
//
struct aggregated_latency_one_terminal {
    double sum_latency;
    unsigned int total_msgs;
};

struct latency_surrogate {
    struct aggregated_latency_one_terminal aggregated_latency_for_all;
    unsigned int num_terminals;
    struct aggregated_latency_one_terminal aggregated_latency[];
};

static void init_pred(struct latency_surrogate * data, tw_lp * lp, unsigned int src_terminal) {
    (void) lp;
    (void) src_terminal;
    assert(data->num_terminals == 0);
    assert(data->aggregated_latency_for_all.sum_latency == 0);
    assert(data->aggregated_latency_for_all.total_msgs == 0);
    assert(data->aggregated_latency[0].sum_latency == 0);
    assert(data->aggregated_latency[0].total_msgs == 0);

    data->num_terminals = total_terminals;
}

static void feed_pred(struct latency_surrogate * data, tw_lp * lp, unsigned int src_terminal, struct packet_start * start, struct packet_end * end) {
    (void) lp;
    (void) src_terminal;

    if (start->travel_start_time < ignore_until) {
        return;
    }

    unsigned int const dest_terminal = start->dfdally_dest_terminal_id;
    double const latency = end->travel_end_time - start->travel_start_time;
    assert(dest_terminal < data->num_terminals);

    data->aggregated_latency[dest_terminal].sum_latency += latency;
    data->aggregated_latency[dest_terminal].total_msgs++;

    data->aggregated_latency_for_all.sum_latency += latency;
    data->aggregated_latency_for_all.total_msgs++;
}

static double predict_latency(struct latency_surrogate * data, tw_lp * lp, unsigned int src_terminal, struct packet_start * packet_dest) {
    (void) lp;

    unsigned int const dest_terminal = packet_dest->dfdally_dest_terminal_id;
    assert(dest_terminal < data->num_terminals);

    // In case we have any data to determine the average for a specific terminal
    unsigned int const total_datapoints = data->aggregated_latency[dest_terminal].total_msgs;
    if (total_datapoints > 0) {
        double const sum_latency = data->aggregated_latency[dest_terminal].sum_latency;
        return sum_latency / total_datapoints;
    }

    // If no information for that terminal exists, use average from all message
    unsigned int const total_total_datapoints = data->aggregated_latency_for_all.total_msgs;
    if (total_total_datapoints > 0) {
        double const sum_latency = data->aggregated_latency_for_all.sum_latency;
        return sum_latency / total_total_datapoints;
    }

    // otherwise, we have no data to approximate the latency
    tw_error(TW_LOC, "The terminal %u doesn't have any packet delay information available to predict future packet latency!\n", src_terminal);
    return -1.0;

    // TODO(elkin): this (below) is wrong, bad bad. I'm not entirely sure how to do this rn in a non-hardcoded manner, but given time, this should be left in better terms
    // THIS HAS BEEN HARDCODED FOR THE CASE OF 72-node DRAGONFLY

    //// Otherwise, use "sensible" results from another simulation
    //// This assumes the network is a 72 nodes 1D-DragonFly (9 groups, with 4 routers, and 2 terminals per router)
    //// source and destination share the same router
    //if (src_terminal / 2 == dest_terminal / 2) {
    //    return 2108.74;
    //}
    //// source and destination are in the same group
    //else if (src_terminal / 8 == dest_terminal / 8) {
    //    return 2390.13;
    //}
    //// source and destination are in different groups
    //else {
    //    return 4162.77;
    //}
}

static void predict_latency_rc(struct latency_surrogate * data, tw_lp * lp) {
    (void) data;
    (void) lp;
}


struct packet_latency_predictor average_latency_predictor = {
    .init              = (init_pred_f) init_pred,
    .feed              = (feed_pred_f) feed_pred,
    .predict           = (predict_pred_f) predict_latency,
    .predict_rc        = (predict_pred_rc_f) predict_latency_rc,
    .predictor_data_sz = sizeof(struct latency_surrogate) + 72 * sizeof(struct aggregated_latency_one_terminal)
};
//
// === END OF Average packet latency functionality


// === Director functionality
//
struct director_data my_director_data;

static struct {
    size_t current_i;
    size_t total;
    double * time_stampts; // list of precise timestamps at which to switch
} switch_at;


void director_fun(tw_pe * pe) {
    static int i = 0;
    if (g_tw_mynode == 0) {
        if (DEBUG_DIRECTOR == 2) {
            printf(".");
            fflush(stdout);
        }
        if (DEBUG_DIRECTOR == 3) {
            printf("GVT %d at %f in %s\n", i++, pe->GVT_sig.recv_ts,
                    my_director_data.is_surrogate_on() ? "surrogate-mode" : "high-definition");
        }
    }

    // Do not process if the simulation ended
    if (pe->GVT_sig.recv_ts >= g_tw_ts_end) {
        return;
    }

    // Switching to and from surrogate mode at times determined by `switch_at`
    if (switch_at.current_i < switch_at.total) {
        double const now = pe->GVT_sig.recv_ts;
        double const next_switch = switch_at.time_stampts[switch_at.current_i];
        if (now > next_switch) {
            if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
                if (DEBUG_DIRECTOR == 2) {
                    printf("\n");
                }
                printf("switching at %g", now);
            }
            my_director_data.switch_surrogate();
            if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
                printf(" to %s\n", my_director_data.is_surrogate_on() ? "surrogate" : "vanilla");
            }
            switch_at.current_i++;
        }
    }
}
//
// === END OF Director functionality


// === All things Surrogate Configuration
void surrogate_config(
        const char * anno,
        const struct director_data d,
        const int total_terminals_,
        struct packet_latency_predictor ** pl_pred
) {
    // This is the only place where the director data should be setup
    my_director_data = d;
    total_terminals = total_terminals_;

    // Determining which director mode to set up
    char director_mode[MAX_NAME_LENGTH];
    director_mode[0] = '\0';
    configuration_get_value(&config, "SURROGATE", "director_mode", anno, director_mode, MAX_NAME_LENGTH);
    if (strcmp(director_mode, "at-fixed-virtual-times") == 0) {
        if(g_tw_mynode == 0) {
            fprintf(stderr, "\nSurrogate activated switching at fixed virtual times: ");
        }

        // Loading timestamps
        char **timestamps;
        size_t len;
        configuration_get_multivalue(&config, "SURROGATE", "fixed_switch_timestamps", anno, &timestamps, &len);

        switch_at.current_i = 0;
        switch_at.total = len;
        switch_at.time_stampts = malloc(len * sizeof(double));

        for (size_t i = 0; i < len; i++) {
            errno = 0;
            switch_at.time_stampts[i] = strtod(timestamps[i], NULL);
            if (errno == ERANGE || errno == EILSEQ){
                tw_error(TW_LOC, "Sequence `%s' could not be succesfully interpreted as a _double_.", timestamps[i]);
            }

            if(g_tw_mynode == 0) {
                fprintf(stderr, "%g%s", switch_at.time_stampts[i], i == len-1 ? "" : ", ");
            }
        }
        if(g_tw_mynode == 0) {
            fprintf(stderr, "\n");
        }

        // Injecting into ROSS function to be called at GVT
        g_tw_gvt_arbitrary_fun = director_fun;

        // freeing timestamps before it dissapears
        for (size_t i = 0; i < len; i++) {
            free(timestamps[i]);
        }
        free(timestamps);
    } else {
        tw_error(TW_LOC, "Unknown director mode `%s`", director_mode);
    }

    // Determining which predictor to set up and return
    char latency_pred_name[MAX_NAME_LENGTH];
    latency_pred_name[0] = '\0';
    configuration_get_value(&config, "SURROGATE", "packet_latency_predictor", anno, latency_pred_name, MAX_NAME_LENGTH);
    if (strcmp(latency_pred_name, "average") == 0) {
        *pl_pred = &average_latency_predictor;

        // Finding out whether to ignore some packet latencies
        int rc = configuration_get_value_double(&config, "SURROGATE", "ignore_until", anno, &ignore_until);
        if (rc) {
            ignore_until = -1; // any negative number disables ignore_until, all packet latencies will be considered
        }
        if (g_tw_mynode == 0) {
            fprintf(stderr, "Enabling average packet latency predictor with ignore_until=%g\n", ignore_until);
        }
    } else {
        tw_error(TW_LOC, "Unknown predictor for packet latency `%s`", latency_pred_name);
    }

    //my_director_data.switch_surrogate();
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        fprintf(stderr, "Simulation starting on %s mode\n", my_director_data.is_surrogate_on() ? "surrogate" : "vanilla");
    }
}
// === END OF All things Surrogate Configuration
