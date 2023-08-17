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

// A simple macro to clarify code a bit
#define PRINTF_ONCE(...) if (g_tw_mynode == 0) { fprintf(stderr, __VA_ARGS__); }

// Basic level of debugging is 1. It should be always turned on
// because it tells us when a switch to or from surrogate-mode happened.
// It can be deactivated (set to 0) if it ends up being too obnoxious
// Level 0: don't show anything
// Level 1: show when surrogate-mode is activated and deactivated
// Level 2: level 1 and some information at each GVT
// Level 3: level 1 and show extended information at each GVT
#define DEBUG_DIRECTOR 1

// Global variables
bool freeze_network_on_switch = true;
static bool is_surrogate_configured = false;
static double surrogate_switching_time = 0.0;
static double ignore_until = 0;
static struct surrogate_config surr_config = {0};

// === Average packet latency functionality
//
struct aggregated_latency_one_terminal {
    double sum_latency;
    unsigned int total_msgs;
};

struct latency_surrogate {
    struct aggregated_latency_one_terminal aggregated_next_packet_delay;
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
    assert(data->aggregated_next_packet_delay.total_msgs == 0);
    assert(data->aggregated_next_packet_delay.sum_latency == 0);

    data->num_terminals = surr_config.total_terminals;
}

static void feed_pred(struct latency_surrogate * data, tw_lp * lp, unsigned int src_terminal, struct packet_start const * start, struct packet_end const * end) {
    (void) lp;
    (void) src_terminal;

    if (start->travel_start_time < ignore_until) {
        return;
    }

    unsigned int const dest_terminal = start->dfdally_dest_terminal_id;
    double const latency = end->travel_end_time - start->travel_start_time;
    assert(dest_terminal < data->num_terminals);
    assert(end->travel_end_time > start->travel_start_time);

    // For average latency per terminal
    data->aggregated_latency[dest_terminal].sum_latency += latency;
    data->aggregated_latency[dest_terminal].total_msgs++;

    // For average total latency (used in case there is no data for a specific node)
    data->aggregated_latency_for_all.sum_latency += latency;
    data->aggregated_latency_for_all.total_msgs++;

    // We ignore the delay if there are no more packets in the queue
    if (start->is_there_another_pckt_in_queue) {
        data->aggregated_next_packet_delay.sum_latency += end->next_packet_delay;
        data->aggregated_next_packet_delay.total_msgs ++;
    }
}

static struct packet_end predict_latency(struct latency_surrogate * data, tw_lp * lp, unsigned int src_terminal, struct packet_start const * packet_dest) {
    (void) lp;

    unsigned int const dest_terminal = packet_dest->dfdally_dest_terminal_id;
    assert(dest_terminal < data->num_terminals);

    unsigned int const total_total_datapoints = data->aggregated_latency_for_all.total_msgs;
    if (total_total_datapoints == 0) {
        // otherwise, we have no data to approximate the latency
        tw_error(TW_LOC, "Terminal %u doesn't have any packet delay information available to predict future packet latency!\n", src_terminal);
        return (struct packet_end) {
            .travel_end_time = -1.0,
            .next_packet_delay = -1.0,
        };
    }

    // In case we have any data to determine the average for a specific terminal
    unsigned int const total_datapoints_for_term = data->aggregated_latency[dest_terminal].total_msgs;
    double latency = -1.0;
    if (total_datapoints_for_term > 0) {
        latency = data->aggregated_latency[dest_terminal].sum_latency / total_datapoints_for_term;
    } else {
        // If no information for that terminal exists, use average from all message
        latency = data->aggregated_latency_for_all.sum_latency / total_total_datapoints;
    }
    assert(latency >= 0);

    double const next_packet_delay =
        data->aggregated_next_packet_delay.sum_latency / data->aggregated_next_packet_delay.total_msgs;
    return (struct packet_end) {
        .travel_end_time = packet_dest->travel_start_time + latency,
        .next_packet_delay = next_packet_delay,
    };
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


//static void offset_future_events_in_causality_list(double switch_offset, tw_event_sig gvt) {
//    (void) switch_offset;
//    (void) gvt;
//    int events_processed = 0;
//    int events_modified = 0;
//    for (unsigned int i = 0; i < g_tw_nkp; i++) {
//        tw_kp * const this_kp = g_tw_kp[i];
//
//        //assert(this_kp->pevent_q.size == 0);
//        // All events in pevent_q are sent into the future
//        assert((this_kp->pevent_q.tail == NULL) == (this_kp->pevent_q.size == 0));
//        tw_event * cur_event = this_kp->pevent_q.tail;
//        while (cur_event) {
//            if (!is_workload_event(cur_event) && tw_event_sig_compare(cur_event->sig, gvt) > 0) {
//                cur_event->recv_ts += switch_offset;
//                cur_event->sig.recv_ts = cur_event->recv_ts;
//                events_modified++;
//            }
//
//            cur_event = cur_event->prev;
//            events_processed++;
//        }
//    }
//    if (DEBUG_DIRECTOR > 1 && g_tw_mynode == 0) {
//        printf("PE %lu: Total events from causality modified %d (from total processed %d)\n", g_tw_mynode, events_modified, events_processed);
//    }
//}


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


//static tw_event_sig find_sig_smallest_larger_than(double switch_, tw_kp * kp, tw_event_sig gvt) {
//    //printf("Just testing, I'm here! size=%d\n", kp->pevent_q.size);
//    tw_event * cur_event = kp->pevent_q.tail;
//    while (cur_event) {
//        //printf("Current timestamp to rollback (%e) and gvt (%e)\n", cur_event->sig.recv_ts, gvt.recv_ts);
//        if (tw_event_sig_compare(cur_event->sig, gvt) < 0 && switch_ <= cur_event->sig.recv_ts) {
//            gvt = cur_event->sig;
//        }
//        cur_event = cur_event->prev;
//    }
//    return gvt;
//}


#ifdef USE_RAND_TIEBREAKER
static void rollback_and_cancel_events_pe(tw_pe * pe, tw_event_sig gvt_sig) {
    tw_stime const gvt = gvt_sig.recv_ts;
    // Backtracking the simulation to GVT
    for (unsigned int i = 0; i < g_tw_nkp; i++) {
        tw_kp_rollback_to_sig(g_tw_kp[i], gvt_sig);
    }
    assert(tw_event_sig_compare(pe->GVT_sig, gvt_sig) == 0);
    assert(pe->GVT_sig.recv_ts == gvt);  // redundant but needed because compiler cries that gvt is never used
#else
static void rollback_and_cancel_events_pe(tw_pe * pe, tw_stime gvt) {
    // Backtracking the simulation to GVT
    for (unsigned int i = 0; i < g_tw_nkp; i++) {
        tw_kp_rollback_to(g_tw_kp[i], gvt);
    }
    assert(pe->GVT == gvt);
#endif

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
            printf("PE %lu: Time stamp at the end of GVT time: %e - AVL-tree sized: %d\n", g_tw_mynode, gvt, pe->avl_tree_size);
        }
    } while (does_any_pe(pe->cancel_q != NULL) || does_any_pe(pe->event_q.size != 0));

    tw_pe_fossil_collect();

    if (DEBUG_DIRECTOR > 1) {
        printf("PE %lu: All events rolledbacked and cancelled\n", g_tw_mynode);
    }
}

#ifdef USE_RAND_TIEBREAKER
static void shift_events_to_future_pe(tw_pe * pe, tw_event_sig gvt_sig) {
    tw_stime gvt = gvt_sig.recv_ts;  // pe->GVT_sig.recv_ts;
#else
static void shift_events_to_future_pe(tw_pe * pe, tw_stime gvt) {
#endif
    tw_event * next_event = tw_pq_dequeue(pe->pq);

    // If there aren't any events left to process, the simulation has already finished and we have nothing to do
    if (next_event == NULL) {
        return;
    }

    // We have to put the events back into the queue after we switch back, but if we never
    // switch back they will never get to be processed and thus we can clean them
    double switch_offset = g_tw_ts_end;
    if (switch_at.current_i + 1 < switch_at.total) {
        double const next_switch = switch_at.time_stampts[switch_at.current_i + 1];
        double const pre_switch_time = gvt;
        switch_offset = next_switch - pre_switch_time;
        assert(pre_switch_time < next_switch);
        //printf("gvt=%f next_switch=%f switch_offset=%f\n", pre_switch_time, next_switch, switch_offset);
    }

    tw_event * dequed_events = NULL; // Linked list of workload events, to be placed again in the queue
    int events_dequeued = 0;  // for stats on code correctness
    // Traversing all events stored in the queue
    while (next_event) {
        // Filtering events to freeze
        assert(next_event->prev == NULL);
#ifdef USE_RAND_TIEBREAKER
        assert(tw_event_sig_compare(next_event->sig, gvt_sig) >= 0);
#else
        assert(next_event->recv_ts >= gvt);
#endif

        // finding out lp type
        char const * lp_type_name;
        int rep_id, offset; // unused
        codes_mapping_get_lp_info2(next_event->dest_lpid, NULL, &lp_type_name, NULL, &rep_id, &offset);
        struct lp_types_switch const * const lp_type_switch = get_type_switch(lp_type_name);

        // shifting time stamps to the future for events to freeze
        if (lp_type_switch && lp_type_switch->should_event_be_frozen
                && lp_type_switch->should_event_be_frozen(next_event->dest_lp, next_event)) {
#ifdef USE_RAND_TIEBREAKER
            assert(next_event->recv_ts == next_event->sig.recv_ts);
            next_event->recv_ts += switch_offset;
            next_event->sig.recv_ts = next_event->recv_ts;
        }
        assert(next_event->recv_ts >= g_tw_trigger_arbitrary_fun.sig_at.recv_ts);
#else
            next_event->recv_ts += switch_offset;
        }
        assert(next_event->recv_ts >= g_tw_trigger_arbitrary_fun.at);
#endif

        // store event in deque_events to inject immediately back to the queue
        next_event->prev = dequed_events;
        dequed_events = next_event;
        events_dequeued++;

        next_event = tw_pq_dequeue(pe->pq);
    }

    int events_enqueued = 0;
    // Reinjecting events into simulation
    while (dequed_events) {
        tw_event * const prev_event = dequed_events;
        dequed_events = dequed_events->prev;
        prev_event->prev = NULL;
        tw_pq_enqueue(pe->pq, prev_event);

        events_enqueued++;
    }

    if (DEBUG_DIRECTOR > 1) {
        printf("PE %lu: Discrepancy on number of events processed %d (%d dequeued and %d enqueued)\n",
                g_tw_mynode, events_dequeued - events_enqueued, events_dequeued, events_enqueued);
    }

    // shifting time stamps of events in causality list (one list per KP)
    // offset_future_events_in_causality_list(switch_offset, gvt);
}


// Returns an array of size `g_tw_nlp`, where each element is a null-terminated
// array containing all the events that each LP has for processing
static tw_event *** order_events_per_lps(tw_pe * pe) {
    // 0. Create array for linked list of size g_tw_nlp to store events per lp
    tw_event ** lp_queue_events = (tw_event **) calloc(g_tw_nlp, sizeof(tw_event *));
    // 0b. Create simple array (size g_tw_lp) to store number of events per lp
    size_t * num_lp_queue_events = (size_t *) calloc(g_tw_nlp, sizeof(size_t));

    // 1. loop extracting events from queue
    //   a. check from which local lp does the event belong
    //   b. add event to reversed linked-list of given lp and increase lp counter
    tw_event * next_event = tw_pq_dequeue(pe->pq);
    size_t events_dequeued = 0;
    while (next_event) {
        // Filtering events to freeze
        assert(next_event->prev == NULL);

        // finding out lp type
        assert(tw_getlocal_lp(next_event->dest_lpid) == next_event->dest_lp);
        tw_lpid const lpid = next_event->dest_lp->id;

        // store event in lp_queue_events
        next_event->prev = lp_queue_events[lpid];
        lp_queue_events[lpid] = next_event;
        num_lp_queue_events[lpid]++;
        events_dequeued++;

        next_event = tw_pq_dequeue(pe->pq);
    }

    // 2. create array (triple pointer type, **) of size `g_tw_nlp + total events`
    //    to store events per lp, null-terminated
    tw_event *** lps_events = (tw_event ** *) calloc(g_tw_nlp, sizeof(tw_event **));
    tw_event ** all_events_mem = (tw_event * *) calloc(g_tw_nlp + events_dequeued, sizeof(tw_event *));

    // 3. loop through each linked-list insert each event back into the
    //   queue and store address copy into lp array
    size_t event_i = 0;
    for (size_t lpid = 0; lpid < g_tw_nlp; lpid++) {
        lps_events[lpid] = &all_events_mem[event_i];

        tw_event * dequed_events = lp_queue_events[lpid];
        while (dequed_events) {
            // event address copy
            all_events_mem[event_i] = dequed_events;

            // placing back into queue
            tw_event * const prev_event = dequed_events;
            dequed_events = dequed_events->prev;
            prev_event->prev = NULL;
            tw_pq_enqueue(pe->pq, prev_event);

            event_i++;
        }
        event_i++;
    }
    assert(event_i == g_tw_nlp + events_dequeued);

    assert(g_tw_nlp > 0 && lps_events[0] == all_events_mem);
    free(lp_queue_events);
    free(num_lp_queue_events);
    return lps_events;
}


// Switching from a (vanilla) high-def simulation to surrogate mode
// consists of:
// - Cancel all events that have to be cancelled and clean everything
// - Looking at all events in the PE, "freezing" those in the network model
//   and letting the workload events be processed further
// - Going through every LP and calling their respective functions
#ifdef USE_RAND_TIEBREAKER
static void events_high_def_to_surrogate_switch(tw_pe * pe, tw_event_sig gvt) {
#else
static void events_high_def_to_surrogate_switch(tw_pe * pe, tw_stime gvt) {
#endif
    if (g_tw_synchronization_protocol != OPTIMISTIC && g_tw_synchronization_protocol != SEQUENTIAL) {
        tw_error(TW_LOC, "Sorry, sending packets to the future hasn't been implement in this mode");
    }

    tw_event *** lps_events = order_events_per_lps(pe);
    shift_events_to_future_pe(pe, gvt);

    // Going through all LPs in PE and running their specific functions
    for (tw_lpid local_lpid = 0; local_lpid < g_tw_nlp; local_lpid++) {
        tw_lp * const lp = g_tw_lp[local_lpid];
        assert(local_lpid == lp->id);

        // Modifying current time for LPs (technically, KPs) so that they
        // coincide with current GVT (the current GVT often does not
        // correspond to the (last) time stored in KPs).
#ifdef USE_RAND_TIEBREAKER
        lp->kp->last_sig = gvt;
#else
        lp->kp->last_time = gvt;
#endif

        char const * lp_type_name;
        int rep_id, offset; // unused
        codes_mapping_get_lp_info2(lp->gid, NULL, &lp_type_name, NULL, &rep_id, &offset);
        bool const is_lp_modelnet = strncmp("modelnet_", lp_type_name, 9) == 0;
        struct lp_types_switch const * const lp_type_switch = get_type_switch(lp_type_name);

        if (lp_type_switch) {
            if (lp_type_switch->trigger_idle_modelnet) {
                assert(is_lp_modelnet);
                model_net_method_switch_to_surrogate_lp(lp);
            }
            if (lp_type_switch->surrogate_to_highdef) {
                if (is_lp_modelnet) {
                    model_net_method_call_inner(lp, lp_type_switch->highdef_to_surrogate, lps_events[local_lpid]);
                } else {
                    lp_type_switch->highdef_to_surrogate(lp->cur_state, lp, lps_events[local_lpid]);
                }
            }
        }
    }

    // This will force a global update on all the new remote events (instead of waiting until the next GVT cycle to update events to process)
    if (g_tw_synchronization_protocol == OPTIMISTIC) {
        rollback_and_cancel_events_pe(pe, gvt);
    }

    assert(lps_events[0] != NULL);
    free(lps_events[0]);
    free(lps_events);
}


#ifdef USE_RAND_TIEBREAKER
static void events_surrogate_to_high_def_switch(tw_pe * pe, tw_event_sig gvt) {
#else
static void events_surrogate_to_high_def_switch(tw_pe * pe, tw_stime gvt) {
#endif
    (void) pe;

    // Going through all LPs in PE and running their specific functions
    for (tw_lpid local_lpid = 0; local_lpid < g_tw_nlp; local_lpid++) {
        tw_lp * const lp = g_tw_lp[local_lpid];
        assert(local_lpid == lp->id);

        // Modifying current time for LPs (technically, KPs) so that they
        // coincide with current GVT (the current GVT often does not
        // correspond to the (last) time stored in KPs).
#ifdef USE_RAND_TIEBREAKER
        tw_event_sig const previous_sig = lp->kp->last_sig;
        lp->kp->last_sig = gvt;
#else
        tw_stime const previous_time = lp->kp->last_time;
        lp->kp->last_time = gvt;
#endif

        char const * lp_type_name;
        int rep_id, offset; // unused
        codes_mapping_get_lp_info2(lp->gid, NULL, &lp_type_name, NULL, &rep_id, &offset);
        bool const is_lp_modelnet = strncmp("modelnet_", lp_type_name, 9) == 0;
        struct lp_types_switch const * const lp_type_switch = get_type_switch(lp_type_name);

        if (lp_type_switch) {
            if (lp_type_switch->trigger_idle_modelnet) {
                assert(is_lp_modelnet);
                model_net_method_switch_to_highdef_lp(lp);
            }
            if (lp_type_switch->surrogate_to_highdef) {
                if (is_lp_modelnet) {
                    model_net_method_call_inner(lp, lp_type_switch->surrogate_to_highdef, NULL);
                } else {
                    lp_type_switch->surrogate_to_highdef(lp->cur_state, lp, NULL);
                }
            }
        }

#ifdef USE_RAND_TIEBREAKER
        lp->kp->last_sig = previous_sig;
#else
        lp->kp->last_time = previous_time;
#endif
    }
}


#ifdef USE_RAND_TIEBREAKER
static void director_fun(tw_pe * pe, tw_event_sig gvt_sig) {
    tw_stime const gvt = gvt_sig.recv_ts;
#else
static void director_fun(tw_pe * pe, tw_stime gvt) {
#endif
    assert(is_surrogate_configured);

    static int i = 0;
    if (g_tw_mynode == 0) {
        if (DEBUG_DIRECTOR == 2) {
            printf(".");
            fflush(stdout);
        }
        if (DEBUG_DIRECTOR == 3) {
            printf("GVT %d at %f in %s arbitrary-fun-status=", i++, gvt,
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
#ifdef USE_RAND_TIEBREAKER
    assert((g_tw_synchronization_protocol == SEQUENTIAL) || (pe->GVT_sig.recv_ts == gvt));
#else
    assert((g_tw_synchronization_protocol == SEQUENTIAL) || (pe->GVT == gvt));
#endif

    // Do not process if the simulation ended
    if (gvt >= g_tw_ts_end) {
        return;
    }

    // Detecting if we are going to switch
    if (switch_at.current_i < switch_at.total
            && g_tw_trigger_arbitrary_fun.active == ARBITRARY_FUN_triggered) {
        double const switch_time = switch_at.time_stampts[switch_at.current_i];
#ifdef USE_RAND_TIEBREAKER
        assert(g_tw_trigger_arbitrary_fun.sig_at.recv_ts == switch_at.time_stampts[switch_at.current_i]);
#else
        assert(g_tw_trigger_arbitrary_fun.at == switch_at.time_stampts[switch_at.current_i]);
#endif
        assert(gvt >= switch_time);  // current gvt shouldn't be that far ahead from the point we wanted to trigger it
    } else {
        return;
    }

    double const start = tw_clock_read();
    // Asking the director/model to switch
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        if (DEBUG_DIRECTOR == 2) {
            printf("\n");
        }
        printf("Switching at %g", gvt);
    }
    // Rollback if in optimistic mode
#ifdef USE_RAND_TIEBREAKER
    if (g_tw_synchronization_protocol == OPTIMISTIC) {
        assert(tw_event_sig_compare(pe->GVT_sig, gvt_sig) == 0);
        rollback_and_cancel_events_pe(pe, gvt_sig);
        //assert(tw_event_sig_compare(pe->GVT_sig, gvt_sig) <= 0);
        assert(tw_event_sig_compare(pe->GVT_sig, gvt_sig) == 0);
    }
#else
    if (g_tw_synchronization_protocol == OPTIMISTIC) {
        assert(pe->GVT == gvt);
        rollback_and_cancel_events_pe(pe, gvt);
        //assert(tw_event_sig_compare(pe->GVT_sig, gvt) <= 0);
        assert(pe->GVT == gvt);
    }
#endif
    surr_config.director.switch_surrogate();
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        printf(" to %s\n", surr_config.director.is_surrogate_on() ? "surrogate" : "high-fidelity");
    }

    // "Freezing" network events and activating LP's switch functions
    if (freeze_network_on_switch) {
        if (surr_config.director.is_surrogate_on()) {
            model_net_method_switch_to_surrogate();
#ifdef USE_RAND_TIEBREAKER
            events_high_def_to_surrogate_switch(pe, gvt_sig);
#else
            events_high_def_to_surrogate_switch(pe, gvt);
#endif
        } else {
            model_net_method_switch_to_highdef();
#ifdef USE_RAND_TIEBREAKER
            events_surrogate_to_high_def_switch(pe, gvt_sig);
#else
            events_surrogate_to_high_def_switch(pe, gvt);
#endif
        }
    }

    // Activating next switch
    if (++switch_at.current_i < switch_at.total) {
        double const next_switch = switch_at.time_stampts[switch_at.current_i];
        // Setting trigger for next switch
#ifdef USE_RAND_TIEBREAKER
        tw_event_sig time_stamp = {0};
        time_stamp.recv_ts = next_switch;
        //printf("Adding a trigger to activate next switch!\n");
        tw_trigger_arbitrary_fun_at(time_stamp);
#else
        //printf("Adding a trigger to activate next switch!\n");
        tw_trigger_arbitrary_fun_at(next_switch);
#endif
    }

    if (DEBUG_DIRECTOR == 1 && g_tw_mynode == 0) {
        printf("Switch completed!\n");
    }
    if (DEBUG_DIRECTOR > 1) {
        printf("PE %lu: Switch completed!\n", g_tw_mynode);
    }
    surrogate_switching_time += tw_clock_read() - start;
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
    is_surrogate_configured = true;

    // This is the only place where the director data should be loaded and set up
    surr_config = *sc;

    // Determining which director mode to set up
    char director_mode[MAX_NAME_LENGTH];
    director_mode[0] = '\0';
    configuration_get_value(&config, "SURROGATE", "director_mode", anno, director_mode, MAX_NAME_LENGTH);
    if (strcmp(director_mode, "at-fixed-virtual-times") == 0) {
        PRINTF_ONCE("\nSurrogate activated switching at fixed virtual times: ");

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

            PRINTF_ONCE("%g%s", switch_at.time_stampts[i], i == len-1 ? "" : ", ");
        }
        PRINTF_ONCE("\n");

        // Injecting into ROSS the function to be called at GVT and the instant in time to trigger GVT
        g_tw_gvt_arbitrary_fun = director_fun;

#ifdef USE_RAND_TIEBREAKER
        tw_event_sig time_stamp = {0};
        time_stamp.recv_ts = switch_at.time_stampts[0];
        tw_trigger_arbitrary_fun_at(time_stamp);
#else
        tw_trigger_arbitrary_fun_at(switch_at.time_stampts[0]);
#endif

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
    if (*latency_pred_name) {
        if (strcmp(latency_pred_name, "average") == 0) {
            *pl_pred = &average_latency_predictor;

            // Finding out whether to ignore some packet latencies
            int rc = configuration_get_value_double(&config, "SURROGATE", "ignore_until", anno, &ignore_until);
            if (rc) {
                ignore_until = -1; // any negative number disables ignore_until, all packet latencies will be considered
                PRINTF_ONCE("Enabling average packet latency predictor\n");
            } else {
                PRINTF_ONCE("Enabling average packet latency predictor with ignore_until=%g\n", ignore_until);
            }
        } else {
            tw_error(TW_LOC, "Unknown predictor for packet latency `%s` (possibilities include: average)", latency_pred_name);
        }
    } else {
        *pl_pred = &average_latency_predictor;
        PRINTF_ONCE("Enabling average packet latency predictor (default behaviour)\n");
    }

    // Determining which predictor to set up and return
    char network_treatment_name[MAX_NAME_LENGTH];
    network_treatment_name[0] = '\0';
    configuration_get_value(&config, "SURROGATE", "network_treatment_on_switch", anno, network_treatment_name, MAX_NAME_LENGTH);
    if (*network_treatment_name) {
        if (strcmp(network_treatment_name, "freeze") == 0) {
            freeze_network_on_switch = true;
            PRINTF_ONCE("The network will be frozen on switch to surrogate\n");
        } else if (strcmp(network_treatment_name, "nothing") == 0) {
            freeze_network_on_switch = false;
            PRINTF_ONCE("The network will be left alone on switch to surrogate (it will run on the background until it empties by itself)\n");
        } else {
            tw_error(TW_LOC, "Unknown network treatment `%s` (possibilities include: frezee or nothing)", network_treatment_name);
        }
    } else {
        freeze_network_on_switch = true;
        PRINTF_ONCE("The network will be frozen on switch to surrogate (default behaviour)\n");
    }

    //surr_config.director.switch_surrogate();
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        fprintf(stderr, "Simulation starting on %s mode\n", surr_config.director.is_surrogate_on() ? "surrogate" : "vanilla");
    }
}
// === END OF All things Surrogate Configuration


// === Stats!
void print_surrogate_stats(void) {
    if(is_surrogate_configured && g_tw_mynode == 0) {
        printf("\nTotal time spent on switching from and to surrogate-mode: %.4f\n", (double) surrogate_switching_time / g_tw_clock_rate);
    }
}
// === END OF Stats!
