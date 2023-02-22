/**
 * This entire file is in charge of switching a high-definition simulation
 * (a vanilla CODES simulation) into surrogate-mode where a secondary piece
 * of software (a surrogate, a collection of functions), and back.
 * For the switch to happen, we have to inspect some of the "hidden"
 * structure of PDES (ROSS) and thus the code in here relies on a very
 * specific version of ROSS. In a sense, we are abusing the non-documented
 * ABI of ROSS.
 */

#include <assert.h>
#include <codes/configuration.h>
#include <codes/codes_mapping.h>
#include <codes/model-net-lp.h>
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
static double ignore_until = 0;
static struct surrogate_config surr_config = {0};

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

    data->num_terminals = surr_config.total_terminals;
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
    tw_error(TW_LOC, "Terminal %u doesn't have any packet delay information available to predict future packet latency!\n", src_terminal);
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

static struct {
    size_t current_i;
    size_t total;
    double * time_stampts; // list of precise timestamps at which to switch
} switch_at;


// To be treated as a linked list. Use `->next` to access the next event
static bool is_workload_event(tw_event * event) {
    char const * lp_type_name;
    int rep_id, offset; // unused
    codes_mapping_get_lp_info2(event->dest_lpid, NULL, &lp_type_name, NULL, &rep_id, &offset);

    return strncmp("modelnet_", lp_type_name, 9) != 0;
}


static void offset_future_events_in_causality_list(double switch_offset, tw_event_sig gvt) {
    int events_processed = 0;
    int events_modified = 0;
    for (unsigned int i = 0; i < g_tw_nkp; i++) {
        tw_kp * const this_kp = g_tw_kp[i];

        // All events in pevent_q are sent into the future
        assert((this_kp->pevent_q.tail == NULL) == (this_kp->pevent_q.size == 0));
        tw_event * cur_event = this_kp->pevent_q.tail;
        while (cur_event) {
            if (!is_workload_event(cur_event) && tw_event_sig_compare(cur_event->sig, gvt) > 0) {
                cur_event->recv_ts += switch_offset;
                cur_event->sig.recv_ts = cur_event->recv_ts;
                events_modified++;
            }

            cur_event = cur_event->prev;
            events_processed++;
        }
    }
    if (DEBUG_DIRECTOR > 1 && g_tw_mynode == 0) {
        printf("PE %lu: Total events from causality modified %d (from total processed %d)\n", g_tw_mynode, events_modified, events_processed);
    }
}


static struct lp_types_switch const * get_type_switch(char const * const name) {
    for (size_t i = 0; i < surr_config.n_lp_types; i++) {
        //printf("THIS %s and %s\n", surr_config.lp_types[i].lpname, name);
        if (strcmp(surr_config.lp_types[i].lpname, name) == 0) {
            return &surr_config.lp_types[i];
        }
    }
    return NULL;
}


// MPI barrier to determine if anyone has a true value `val`. Returns true if anyone says "TRUE"
static inline bool does_any_pe(bool val) {
    bool global_val;
    if(MPI_Allreduce(&val, &global_val, 1, MPI_C_BOOL, MPI_LOR, MPI_COMM_ROSS) != MPI_SUCCESS) {
        tw_error(TW_LOC, "MPI_Allreduce for custom rollback and cleanup failed");
    }
    return global_val;
}


static void rollback_and_cancel_events_pe(tw_pe * pe) {
    // Backtracking the simulation to GVT
    for (unsigned int i = 0; i < g_tw_nkp; i++) {
        tw_kp_rollback_to_sig(g_tw_kp[i], pe->GVT_sig);
    }

    // Making sure that everything gets cleaned up properly (AVL tree should be empty by the end)
    do {
        if (tw_nnodes() > 1) {
            double const start = tw_clock_read();
            tw_net_read(pe);
            pe->stats.s_net_read += tw_clock_read() - start;
        }

        pe->gvt_status = 1;
        tw_sched_event_q(pe);
        tw_sched_cancel_q(pe);
        tw_gvt_step2(pe);

        if (DEBUG_DIRECTOR > 1) {
            printf("PE %lu: Time stamp at the end of GVT time: %e - AVL-tree sized: %d\n", g_tw_mynode, pe->GVT_sig.recv_ts, pe->avl_tree_size);
        }
    } while (does_any_pe(pe->cancel_q != NULL) || does_any_pe(pe->event_q.size != 0));

    if (DEBUG_DIRECTOR > 1) {
        printf("PE %lu: All events rolledbacked and cancelled\n", g_tw_mynode);
    }
}

static void shift_events_to_future_pe(tw_pe * pe, tw_event_sig gvt) {
    tw_event * next_event = tw_pq_dequeue(pe->pq);

    // If there aren't any events left to process, the simulation has already finished and we have nothing to do
    if (next_event == NULL) {
        return;
    }

    tw_event * frozen_events = NULL;  // Linked list of frozen events
    tw_event * workload_events = NULL; // Linked list of workload events, to be placed again in the queue

    int events_dequeued = 0;
    // Traversing all events stored in the queue
    while (next_event) {
        // Filtering events to freeze
        tw_event * const prev_event = next_event;
        next_event = tw_pq_dequeue(pe->pq);
        assert(prev_event->next == NULL);

        if (is_workload_event(prev_event)) {
            // store event in events to inject immediately back to the queue (in reverse order, because the queue will take the youngest event first)
            if (!workload_events) {
                workload_events = prev_event;
            } else {
                prev_event->prev = workload_events;
                workload_events = prev_event;
            }
        } else {
            // store event in frozen events, to be forwarded to the future
            if (!frozen_events) {
                frozen_events = prev_event;
            } else {
                prev_event->prev = frozen_events;
                frozen_events = prev_event;
            }
        }
        events_dequeued++;
    }

    // We have to put the events back into the queue after we switch back, but if we never
    // switch back they will never get to be processed and thus we can clean them
    double switch_offset = g_tw_ts_end;
    if (switch_at.current_i + 1 < switch_at.total) {
        double const next_switch = switch_at.time_stampts[switch_at.current_i + 1];
        double const pre_switch_time = gvt.recv_ts;  // pe->GVT_sig.recv_ts;
        switch_offset = next_switch - pre_switch_time;
        assert(pre_switch_time < next_switch);
        //printf("gvt=%f next_switch=%f switch_offset=%f\n", pre_switch_time, next_switch, switch_offset);
    }

    int events_enqueued = 0;
    // shifting time stamps of network events to the future
    //printf("Events in the future ");
    while (frozen_events) {
        tw_event * const prev_event = frozen_events;
        frozen_events = frozen_events->prev;

        //printf("%c", tw_event_sig_compare(gvt, prev_event->sig) < 0 ? '.' : 'x');
        if(tw_event_sig_compare(prev_event->sig, gvt) > 0 && !model_net_is_this_base_event(tw_event_data(prev_event))) {
            assert(prev_event->recv_ts == prev_event->sig.recv_ts);
            prev_event->recv_ts += switch_offset;
            prev_event->sig.recv_ts = prev_event->recv_ts;
        }

        prev_event->prev = NULL;
        tw_pq_enqueue(pe->pq, prev_event);
        assert(prev_event->recv_ts >= g_tw_trigger_arbitrary_fun.sig_at.recv_ts);

        events_enqueued++;
    }

    // Reinjecting workload events into simulation
    while (workload_events) {
        tw_event * const prev_event = workload_events;
        workload_events = workload_events->prev;
        prev_event->prev = NULL;
        tw_pq_enqueue(pe->pq, prev_event);

        events_enqueued++;
    }

    if (DEBUG_DIRECTOR > 1 && g_tw_mynode == 0) {
        printf("PE %lu: Discrepancy on number of events processed %d (%d dequeued and %d enqueued)\n",
                g_tw_mynode, events_dequeued - events_enqueued, events_dequeued, events_enqueued);
    }

    // shifting time stamps of events in causality list (one list per KP)
    offset_future_events_in_causality_list(switch_offset, gvt);
}


// Switching from a (vanilla) high-def simulation to surrogate mode
// consists of:
// - Cancel all events that have to be cancelled and clean everything
// - Looking at all events in the PE, "freezing" those in the network model
//   and letting the workload events be processed further
// - Going through every LP and calling their respective functions
static void events_high_def_to_surrogate_switch(tw_pe * pe, tw_event_sig gvt) {
    if (g_tw_synchronization_protocol != OPTIMISTIC && g_tw_synchronization_protocol != SEQUENTIAL) {
        tw_error(TW_LOC, "Sorry, sending packets to the future hasn't been implement in this mode");
    }

    if (g_tw_synchronization_protocol == OPTIMISTIC) {
        assert(tw_event_sig_compare(pe->GVT_sig, gvt) == 0);
        rollback_and_cancel_events_pe(pe);
        //assert(tw_event_sig_compare(pe->GVT_sig, gvt) <= 0);
        assert(tw_event_sig_compare(pe->GVT_sig, gvt) == 0);
    }

    shift_events_to_future_pe(pe, gvt);
    model_net_method_switch_to_surrogate();

    // Going through all LPs in PE and running their specific functions
    for (tw_lpid local_lpid = 0; local_lpid < g_tw_nlp; local_lpid++) {
        tw_lp * const lp = g_tw_lp[local_lpid];
        assert(local_lpid == lp->id);

        // Modifying current time for LPs (technically, KPs) so that they
        // coincide with current GVT (the current GVT often does not
        // correspond to the (last) time stored in KPs).
        lp->kp->last_sig = gvt;

        char const * lp_type_name;
        int rep_id, offset; // unused
        codes_mapping_get_lp_info2(lp->gid, NULL, &lp_type_name, NULL, &rep_id, &offset);
        struct lp_types_switch const * const lp_type_switch = get_type_switch(lp_type_name);

        if (lp_type_switch && lp_type_switch->highdef_to_surrogate) {
            if (lp_type_switch->is_modelnet) {
                model_net_method_switch_to_surrogate_lp(lp);
                model_net_method_call_inner(lp, lp_type_switch->highdef_to_surrogate);
            } else {
                lp_type_switch->highdef_to_surrogate(lp->cur_state, lp);
            }
        }
    }
}


static void events_surrogate_to_high_def_switch(tw_pe * pe, tw_event_sig gvt) {
    (void) pe;
    model_net_method_switch_to_highdef();

    // Going through all LPs in PE and running their specific functions
    for (tw_lpid local_lpid = 0; local_lpid < g_tw_nlp; local_lpid++) {
        tw_lp * const lp = g_tw_lp[local_lpid];
        assert(local_lpid == lp->id);

        // Modifying current time for LPs (technically, KPs) so that they
        // coincide with current GVT (the current GVT often does not
        // correspond to the (last) time stored in KPs).
        tw_event_sig const previous_sig = lp->kp->last_sig;
        lp->kp->last_sig = gvt;

        char const * lp_type_name;
        int rep_id, offset; // unused
        codes_mapping_get_lp_info2(lp->gid, NULL, &lp_type_name, NULL, &rep_id, &offset);
        struct lp_types_switch const * const lp_type_switch = get_type_switch(lp_type_name);

        if (lp_type_switch && lp_type_switch->surrogate_to_highdef) {
            if (lp_type_switch->is_modelnet) {
                model_net_method_switch_to_highdef_lp(lp);
                model_net_method_call_inner(lp, lp_type_switch->surrogate_to_highdef);
            } else {
                lp_type_switch->surrogate_to_highdef(lp->cur_state, lp);
            }
        }

        lp->kp->last_sig = previous_sig;
    }
}


static void director_fun(tw_pe * pe, tw_event_sig gvt) {
    static int i = 0;
    if (g_tw_mynode == 0) {
        if (DEBUG_DIRECTOR == 2) {
            printf(".");
            fflush(stdout);
        }
        if (DEBUG_DIRECTOR == 3) {
            printf("GVT %d at %f in %s arbitrary-fun-status=", i++, gvt.recv_ts,
                    surr_config.director.is_surrogate_on() ? "surrogate-mode" : "high-definition");

            switch (g_tw_trigger_arbitrary_fun.active) {
                case ARBITRARY_FUN_enabled:
                    printf("enabled\n");
                    break;
                case ARBITRARY_FUN_disabled:
                    printf("disabled\n");
                    break;
                case ARBITRARY_FUN_triggered:
                    printf("triggered\n");
                    break;
            }
        }
    }

    // Only in sequential mode pe->GVT does not carry the current gvt, while it does in conservative and optimistic
    assert((g_tw_synchronization_protocol == SEQUENTIAL) || (pe->GVT_sig.recv_ts == gvt.recv_ts));

    // Do not process if the simulation ended
    if (gvt.recv_ts >= g_tw_ts_end) {
        return;
    }

    // Detecting if we are going to switch
    if (switch_at.current_i < switch_at.total
            && g_tw_trigger_arbitrary_fun.active == ARBITRARY_FUN_triggered) {
        double const now = gvt.recv_ts;
        double const switch_time = switch_at.time_stampts[switch_at.current_i];
        assert(g_tw_trigger_arbitrary_fun.sig_at.recv_ts == switch_at.time_stampts[switch_at.current_i]);
        assert(now >= switch_time);  // current gvt shouldn't be that far ahead from the point we wanted to trigger it
    } else {
        return;
    }

    // Asking the director/model to switch
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        if (DEBUG_DIRECTOR == 2) {
            printf("\n");
        }
        printf("Switching at %g", gvt.recv_ts);
    }
    surr_config.director.switch_surrogate();
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        printf(" to %s\n", surr_config.director.is_surrogate_on() ? "surrogate" : "vanilla");
    }

    // "Freezing" network events and activating LP's switch functions
    if (FREEZE_NETWORK_STATE) {
        if (surr_config.director.is_surrogate_on()) {
            events_high_def_to_surrogate_switch(pe, gvt);
        } else {
            events_surrogate_to_high_def_switch(pe, gvt);
        }
    }

    // Activating next switch
    if (++switch_at.current_i < switch_at.total) {
        double const next_switch = switch_at.time_stampts[switch_at.current_i];
        // Setting trigger for next switch
        tw_event_sig time_stamp = {0};
        time_stamp.recv_ts = next_switch;
        //printf("Adding a trigger to activate next switch!\n");
        tw_trigger_arbitrary_fun_at(time_stamp);
    }

    if (DEBUG_DIRECTOR == 1 && g_tw_mynode == 0) {
        printf("Switch completed!\n");
    }
    if (DEBUG_DIRECTOR > 1) {
        printf("PE %lu: Switch completed!\n", g_tw_mynode);
    }
}
//
// === END OF Director functionality


// === All things Surrogate Configuration
void surrogate_configure(
        char const * const anno,
        struct surrogate_config * const sc,
        struct packet_latency_predictor ** pl_pred
) {
    assert(sc);
    assert(0 < sc->n_lp_types && sc->n_lp_types <= MAX_LP_TYPES);

    // This is the only place where the director data should be loaded and set up
    surr_config = *sc;

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

        // Injecting into ROSS the function to be called at GVT and the instant in time to trigger GVT
        g_tw_gvt_arbitrary_fun = director_fun;

        tw_event_sig time_stamp = {0};
        time_stamp.recv_ts = switch_at.time_stampts[0];
        tw_trigger_arbitrary_fun_at(time_stamp);

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

    //surr_config.director.switch_surrogate();
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        fprintf(stderr, "Simulation starting on %s mode\n", surr_config.director.is_surrogate_on() ? "surrogate" : "vanilla");
    }
}
// === END OF All things Surrogate Configuration
