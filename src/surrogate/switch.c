#include <codes/surrogate/init.h>
#include <codes/surrogate/switch.h>
#include <codes/model-net-lp.h>

double surrogate_switching_time = 0.0;
double time_in_surrogate = 0.0;
static double surrogate_time_last = 0.0;

// === Director functionality
//


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
            printf("PE %lu: Time stamp at the end of GVT time: %f - AVL-tree sized: %d\n", g_tw_mynode, gvt, pe->avl_tree_size);
        }
    } while (does_any_pe(pe->cancel_q != NULL) || does_any_pe(pe->event_q.size != 0));

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
void director_switch(tw_pe * pe, tw_event_sig gvt_sig) {
    tw_stime const gvt = gvt_sig.recv_ts;
#else
void director_switch(tw_pe * pe, tw_stime gvt) {
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
        // If the simulation ended and the surrogate is still on, stop timer checking surrogate time
        if (surr_config.director.is_surrogate_on()) {
            time_in_surrogate += tw_clock_read() - surrogate_time_last;
        }
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

    // ---- Past this means that we are in fact switching ----

    double const start = tw_clock_read();
    // Asking the director/model to switch
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        if (DEBUG_DIRECTOR == 2) {
            printf("\n");
        }
        printf("Switching at %f", gvt);
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
    double const end = tw_clock_read();
    surrogate_switching_time += end - start;

    // Determining time in surrogate
    if (surr_config.director.is_surrogate_on()) {
        // Start tracking time spent in surrogate mode
        surrogate_time_last = end;
    } else {
        // We are done tracking time spent in surrogate mode
        time_in_surrogate += start - surrogate_time_last;
    }
}
//
// === END OF Director functionality
