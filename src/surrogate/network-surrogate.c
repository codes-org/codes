#include <codes/surrogate/init.h>
#include <codes/surrogate/network-surrogate.h>
#include <codes/model-net-lp.h>
#include <ross-extern.h>
#include <stdio.h>

#define master_printf(cond, ...) if (cond && g_tw_mynode == 0) { printf(__VA_ARGS__); }

static bool is_network_surrogate_configured = false;
static struct switch_at_struct switch_network_at = {0};
static struct network_surrogate_config net_surr_config = {0};
static bool freeze_network_on_switch = false;
static bool network_director_enabled = false;

// === Frozen events system for separate queue approach
static tw_event *frozen_events_head = NULL;  // Head of frozen events linked list
static double frozen_events_switch_time = 0.0;  // Time when we switched to surrogate mode

// === Director functionality
//

static struct lp_types_switch const * get_type_switch(char const * const name) {
    for (size_t i = 0; i < net_surr_config.n_lp_types; i++) {
        //printf("THIS %s and %s\n", surr_config.lp_types[i].lpname, name);
        if (strcmp(net_surr_config.lp_types[i].lpname, name) == 0) {
            return &net_surr_config.lp_types[i];
        }
    }
    return NULL;
}


static void freeze_events_to_separate_queue_pe(tw_pe * pe) {
#ifdef USE_RAND_TIEBREAKER
    tw_event_sig gvt_sig = pe->GVT_sig;
    tw_stime gvt = gvt_sig.recv_ts;
#else
    tw_stime gvt = pe->GVT;
#endif

    // Store the time when we switch to surrogate mode
    frozen_events_switch_time = gvt;

    tw_event * next_event = tw_pq_dequeue(pe->pq);

    // If there aren't any events left to process, then this PE has nothing to do
    if (next_event == NULL) {
        return;
    }

    tw_event * dequed_events = NULL; // Linked list of non-frozen events, to be placed back in the queue
    int events_processed = 0; // Total events processed from queue
    int events_enqueued = 0;  // Events put back in queue
    int events_frozen = 0;    // Events moved to frozen queue
    int events_deleted = 0;   // Events deleted

    // Traversing all events stored in the queue
    while (next_event) {
        events_processed++;

        // Filtering events to freeze
        assert(next_event->prev == NULL);
#ifdef USE_RAND_TIEBREAKER
        assert(tw_event_sig_compare_ptr(&next_event->sig, &gvt_sig) >= 0);
#else
        assert(next_event->recv_ts >= gvt);
#endif
        if (next_event->event_id && next_event->state.remote) {
            tw_hash_remove(pe->hash_t, next_event, next_event->send_pe);
        }

        // finding out lp type
        char const * lp_type_name;
        int rep_id, offset; // unused
        codes_mapping_get_lp_info2(next_event->dest_lpid, NULL, &lp_type_name, NULL, &rep_id, &offset);
        bool const is_lp_modelnet = strncmp("modelnet_", lp_type_name, 9) == 0;
        struct lp_types_switch const * const lp_type_switch = get_type_switch(lp_type_name);

        // "Processing" event
        if (lp_type_switch && lp_type_switch->check_event_in_queue) {
            if (is_lp_modelnet) {
                model_net_method_call_inner(next_event->dest_lp, (void (*) (void *, tw_lp *, void *))lp_type_switch->check_event_in_queue, next_event);
            } else {
                lp_type_switch->check_event_in_queue(next_event->dest_lp->cur_state, next_event->dest_lp, next_event);
            }
        }

        bool deleted = false;
        bool frozen = false;

        // Check if event should be frozen (moved to separate queue)
        if (lp_type_switch && lp_type_switch->should_event_be_frozen
                && lp_type_switch->should_event_be_frozen(next_event->dest_lp, next_event)) {
            // Add to frozen events linked list (no timestamp manipulation here)
            next_event->prev = frozen_events_head;
            frozen_events_head = next_event;
            frozen = true;
            events_frozen++;
        // deleting event if we need to
        } else if (lp_type_switch && lp_type_switch->should_event_be_deleted
                && lp_type_switch->should_event_be_deleted(next_event->dest_lp, next_event)) {
            tw_event_free(pe, next_event);
            deleted = true;
            events_deleted++;
        }

        // store event in dequed_events to inject immediately back to the queue
        if (!deleted && !frozen) {
             next_event->prev = dequed_events;
             dequed_events = next_event;
        }

        next_event = tw_pq_dequeue(pe->pq);
    }

    // Reinjecting non-frozen events into simulation
    while (dequed_events) {
        tw_event * const prev_event = dequed_events;
        dequed_events = dequed_events->prev;
        prev_event->prev = NULL;
        tw_pq_enqueue(pe->pq, prev_event);

        if (prev_event->event_id && prev_event->state.remote) {
            tw_hash_insert(pe->hash_t, prev_event, prev_event->send_pe);
        }

        events_enqueued++;
    }

    if (DEBUG_DIRECTOR > 0) {
        printf("PE %lu: Processed %d events (%d enqueued, %d frozen, %d deleted)\n",
                g_tw_mynode, events_processed, events_enqueued, events_frozen, events_deleted);
    }

    // Sanity check: processed = enqueued + frozen + deleted
    assert(events_processed == events_enqueued + events_frozen + events_deleted);
}

static void unfreeze_events_from_separate_queue_pe(tw_pe * pe) {
#ifdef USE_RAND_TIEBREAKER
    tw_stime current_gvt = pe->GVT_sig.recv_ts;
#else
    tw_stime current_gvt = pe->GVT;
#endif

    // Calculate offset to adjust timestamps: current_gvt - switch_time
    double time_offset = current_gvt - frozen_events_switch_time;

    int events_restored = 0;

    // Traverse the frozen events linked list and restore them to the main queue
    while (frozen_events_head) {
        tw_event * event_to_restore = frozen_events_head;
        frozen_events_head = frozen_events_head->prev;
        event_to_restore->prev = NULL;

        // Adjust timestamp: original_time + time_spent_in_surrogate
#ifdef USE_RAND_TIEBREAKER
        assert(event_to_restore->recv_ts == event_to_restore->sig.recv_ts);
        event_to_restore->recv_ts += time_offset;
        event_to_restore->sig.recv_ts = event_to_restore->recv_ts;
#else
        event_to_restore->recv_ts += time_offset;
#endif

        // Re-enqueue the event
        tw_pq_enqueue(pe->pq, event_to_restore);

        // Re-add to hash table if it was a remote event
        if (event_to_restore->event_id && event_to_restore->state.remote) {
            tw_hash_insert(pe->hash_t, event_to_restore, event_to_restore->send_pe);
        }

        events_restored++;
    }

    if (DEBUG_DIRECTOR > 0 && events_restored > 0) {
        printf("PE %lu: Restored %d frozen events with time offset %.6f\n",
                g_tw_mynode, events_restored, time_offset);
    }

    // Reset frozen events state
    frozen_events_switch_time = 0.0;
}


// Switching from a (vanilla) high-def simulation to surrogate mode
// consists of:
// - Cancel all events that have to be cancelled and clean everything
// - Looking at all events in the PE, "freezing" those in the network model
//   and letting the workload events be processed further
// - Going through every LP and calling their respective functions
static void events_high_def_to_surrogate_switch(tw_pe * pe) {
#ifdef USE_RAND_TIEBREAKER
    tw_event_sig gvt_sig = pe->GVT_sig;
#else
    tw_stime gvt = pe->GVT;
#endif
    if (g_tw_synchronization_protocol != OPTIMISTIC && g_tw_synchronization_protocol != SEQUENTIAL && g_tw_synchronization_protocol != SEQUENTIAL_ROLLBACK_CHECK) {
        tw_error(TW_LOC, "Sorry, sending packets to the future hasn't been implement in this mode");
    }

    master_printf(DEBUG_DIRECTOR > 1, "PE %lu - AVL size %d (before freezing events)\n", g_tw_mynode, pe->avl_tree_size);
    freeze_events_to_separate_queue_pe(pe);
    master_printf(DEBUG_DIRECTOR > 1, "PE %lu - AVL size %d (after freezing events to separate queue)\n", g_tw_mynode, pe->avl_tree_size);

    // Going through all LPs in PE and running their specific functions
    for (tw_lpid local_lpid = 0; local_lpid < g_tw_nlp; local_lpid++) {
        tw_lp * const lp = g_tw_lp[local_lpid];
        assert(local_lpid == lp->id);

        // Modifying current time for LPs (technically, KPs) so that they
        // coincide with current GVT (the current GVT often does not
        // correspond to the (last) time stored in KPs).
#ifdef USE_RAND_TIEBREAKER
        lp->kp->last_sig = gvt_sig;
#else
        lp->kp->last_time = gvt;
#endif

        char const * lp_type_name;
        int rep_id, offset; // unused
        codes_mapping_get_lp_info2(lp->gid, NULL, &lp_type_name, NULL, &rep_id, &offset);
        bool const is_lp_modelnet = strncmp("modelnet_", lp_type_name, 9) == 0;
        struct lp_types_switch const * const lp_type_switch = get_type_switch(lp_type_name);

        pe->cur_event = pe->abort_event;
        pe->cur_event->caused_by_me = NULL;
#ifdef USE_RAND_TIEBREAKER
        pe->cur_event->sig = pe->GVT_sig;
#else
        pe->cur_event->recv_ts = pe->GVT;
#endif

        if (lp_type_switch) {
            if (lp_type_switch->trigger_idle_modelnet) {
                assert(is_lp_modelnet);
                model_net_method_switch_to_surrogate_lp(lp);
            }
            if (lp_type_switch->highdef_to_surrogate) {
                if (is_lp_modelnet) {
                    model_net_method_call_inner(lp, (void (*) (void *, tw_lp *, void *))lp_type_switch->highdef_to_surrogate, NULL);
                } else {
                    lp_type_switch->highdef_to_surrogate(lp->cur_state, lp, NULL);
                }
            }
        }
    }

    // This will force a global update on all the new remote events (instead of waiting until the next GVT cycle to update events to process)
    if (g_tw_synchronization_protocol == OPTIMISTIC) {
        tw_scheduler_rollback_and_cancel_events_pe(pe);
    }

}


static void events_surrogate_to_high_def_switch(tw_pe * pe) {
#ifdef USE_RAND_TIEBREAKER
    tw_event_sig gvt_sig = pe->GVT_sig;
#else
    tw_stime gvt = pe->GVT;
#endif

    // Restore frozen events back to the main queue with timestamp adjustment
    master_printf(DEBUG_DIRECTOR > 1, "PE %lu - AVL size %d (before injecting events into event queue again)\n", g_tw_mynode, pe->avl_tree_size);
    unfreeze_events_from_separate_queue_pe(pe);
    master_printf(DEBUG_DIRECTOR > 1, "PE %lu - AVL size %d (after defreezing events from separate queue)\n", g_tw_mynode, pe->avl_tree_size);

    // Going through all LPs in PE and running their specific functions
    for (tw_lpid local_lpid = 0; local_lpid < g_tw_nlp; local_lpid++) {
        tw_lp * const lp = g_tw_lp[local_lpid];
        assert(local_lpid == lp->id);

        // Modifying current time for LPs (technically, KPs) so that they
        // coincide with current GVT (the current GVT often does not
        // correspond to the (last) time stored in KPs).
#ifdef USE_RAND_TIEBREAKER
        tw_event_sig const previous_sig = lp->kp->last_sig;
        lp->kp->last_sig = gvt_sig;
#else
        tw_stime const previous_time = lp->kp->last_time;
        lp->kp->last_time = gvt;
#endif

        char const * lp_type_name;
        int rep_id, offset; // unused
        codes_mapping_get_lp_info2(lp->gid, NULL, &lp_type_name, NULL, &rep_id, &offset);
        bool const is_lp_modelnet = strncmp("modelnet_", lp_type_name, 9) == 0;
        struct lp_types_switch const * const lp_type_switch = get_type_switch(lp_type_name);

        pe->cur_event = pe->abort_event;
        pe->cur_event->caused_by_me = NULL;
#ifdef USE_RAND_TIEBREAKER
        pe->cur_event->sig = pe->GVT_sig;
#else
        pe->cur_event->recv_ts = pe->GVT;
#endif

        if (lp_type_switch) {
            if (lp_type_switch->trigger_idle_modelnet) {
                assert(is_lp_modelnet);
                model_net_method_switch_to_highdef_lp(lp);
            }
            if (lp_type_switch->surrogate_to_highdef) {
                if (is_lp_modelnet) {
                    model_net_method_call_inner(lp, (void (*) (void *, tw_lp *, void *))lp_type_switch->surrogate_to_highdef, NULL);
                } else {
                    lp_type_switch->surrogate_to_highdef(lp->cur_state, lp, NULL);
                }
            }
            if (lp_type_switch->reset_predictor) {
                if (is_lp_modelnet) {
                    model_net_method_call_inner(lp, (void (*) (void *, tw_lp *, void *))lp_type_switch->reset_predictor, NULL);
                } else {
                    lp_type_switch->reset_predictor(lp->cur_state, lp, NULL);
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


static void switch_model(tw_pe * pe, bool is_queue_empty) {
    // Rollback if in optimistic mode and the simulation has events yet to process (globally)
    if (g_tw_synchronization_protocol == OPTIMISTIC && !is_queue_empty) {
        tw_scheduler_rollback_and_cancel_events_pe(pe);
    }
    master_printf(DEBUG_DIRECTOR, "Switching to network %s\n", net_surr_config.model.is_surrogate_on() ? "high-fidelity": "surrogate");

    bool const is_surrogate_off = !net_surr_config.model.is_surrogate_on();
    if (is_surrogate_off && is_queue_empty) {
        master_printf(true, "No need to switch to surrogate when the simulation has no events to process\n");
        return;
    }
    net_surr_config.model.switch_surrogate();

    // "Freezing" network events and activating LP's switch functions
    if (freeze_network_on_switch) {
        if (is_surrogate_off) {
            model_net_method_switch_to_surrogate();
            events_high_def_to_surrogate_switch(pe);
        } else {
            model_net_method_switch_to_highdef();
            events_surrogate_to_high_def_switch(pe);
        }
    }
}


void network_director(tw_pe * pe, bool is_queue_empty) {
    assert(is_network_surrogate_configured);
    assert(network_director_enabled);

#ifdef USE_RAND_TIEBREAKER
    tw_stime gvt = pe->GVT_sig.recv_ts;
#else
    tw_stime gvt = pe->GVT;
#endif

    static int i = 0;
    if (g_tw_mynode == 0) {
        if (DEBUG_DIRECTOR == 2) {
            printf(".");
            fflush(stdout);
        }
        if (DEBUG_DIRECTOR == 3) {
            printf("GVT %d at %f in %s\n", i++, gvt,
                    net_surr_config.model.is_surrogate_on() ? "surrogate-mode" : "high-definition");
        }
    }

    // Only in sequential mode pe->GVT does not carry the current gvt, while it does in conservative and optimistic
#ifdef USE_RAND_TIEBREAKER
    assert((g_tw_synchronization_protocol == SEQUENTIAL) || (g_tw_synchronization_protocol == SEQUENTIAL_ROLLBACK_CHECK) || (pe->GVT_sig.recv_ts == gvt));
#else
    assert((g_tw_synchronization_protocol == SEQUENTIAL) || (g_tw_synchronization_protocol == SEQUENTIAL_ROLLBACK_CHECK) || (pe->GVT == gvt));
#endif

    // Do not process if the simulation ended
    if (gvt >= g_tw_ts_end) {
        return;
    }

    // ---- Past this means that we are in fact switching ----
    bool const surrogate_state_pre_switch = net_surr_config.model.is_surrogate_on();

    // Asking the director/model to switch
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        if (DEBUG_DIRECTOR == 2) {
            printf("\n");
        }
        printf("Switching network at %f\n", gvt);
    }

    double const start = tw_clock_read();
    switch_model(pe, is_queue_empty);
    double const end = tw_clock_read();
    surrogate_switching_time += end - start;

    // Setting trigger for next switch
    if (++switch_network_at.current_i < switch_network_at.total) {
        double next_switch = switch_network_at.time_stampts[switch_network_at.current_i];
        tw_trigger_gvt_hook_at(next_switch);
    }

    bool const is_surrogate_on = net_surr_config.model.is_surrogate_on();
    if (is_surrogate_on == surrogate_state_pre_switch) {
        // The surrogate was never switched!
        return;
    }

    master_printf(DEBUG_DIRECTOR == 1, "Network switch completed!\n");
    if (DEBUG_DIRECTOR > 1) {
        printf("PE %lu: Switch completed!\n", g_tw_mynode);
    }

    // Determining time in surrogate
    if (is_surrogate_on) {
        // Start tracking time spent in surrogate mode
        surrogate_time_last = end;
    } else {
        // We are done tracking time spent in surrogate mode
        time_in_surrogate += start - surrogate_time_last;
        surrogate_time_last = 0.0;
    }
}

void network_director_configure(struct network_surrogate_config * sc, struct switch_at_struct * switch_network_at_, bool fnos) {
    is_network_surrogate_configured = true;
    // Injecting into ROSS the function to be called at GVT
    if (switch_network_at_) {
        network_director_enabled = true;
        g_tw_gvt_hook = network_director;
        switch_network_at = *switch_network_at_;
        tw_trigger_gvt_hook_at(switch_network_at.time_stampts[0]);
    }
    net_surr_config = *sc;
    freeze_network_on_switch = fnos;
}

void network_director_finalize(void) {
    if (network_director_enabled) {
        free(switch_network_at.time_stampts);
    }
}

// === Function for application director to use switch to surrogate machinery
void surrogate_switch_network_model(tw_pe * pe, bool is_queue_empty) {
    // Simply expose the existing switch_model function for use by application director
    double const start = tw_clock_read();
    switch_model(pe, is_queue_empty);
    double const end = tw_clock_read();
    surrogate_switching_time += end - start;
}
//
// === END OF Director functionality
// vim: set tabstop=4 shiftwidth=4 expandtab :
