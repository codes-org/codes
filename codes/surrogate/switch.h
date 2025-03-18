#ifndef CODES_SURROGATE_SWITCH_H
#define CODES_SURROGATE_SWITCH_H

/**
 * switch.h -- DIRECTOR FUNCTION in charge of switching back and forth from high-fidelity and surrogate modes
 * Elkin Cruz
 *
 * Copyright (c) 2023 Rensselaer Polytechnic Institute
 */

#include <ross.h>
#include <stdbool.h>
#include "codes/codes_mapping.h"

#ifdef __cplusplus
extern "C" {
#endif

// Time spent switching from high-fidelity to surrogate and viceversa
extern double surrogate_switching_time;
// Total time spent in surrogate mode (between switches)
extern double time_in_surrogate;

// When true (below), the network state will be frozen at switch time (from
// high-def to surrogate) and later reanimated on the switch back (from
// surrogate to high-def). If not, all events will be kept in the network while
// on surrogate mode, which means that the network will vacate completely
extern bool freeze_network_on_switch;

// Functions that director should have access to
typedef void (*switch_surrogate_f) (void); // Switches back and forth from surrogate mode as defined by network model (e.g, by dragonfly-dally.C)
typedef bool (*is_surrogate_on_f) (void); // Switches back and forth from surrogate mode as defined by network model (e.g, by dragonfly-dally.C)

struct director_data {
    switch_surrogate_f  switch_surrogate; // this function switches the model to and from surrogate-mode on a PE basis. It has to be called on all PEs to switch the entire simulation to its surrogate version
    is_surrogate_on_f   is_surrogate_on;  // determines if the model has switched or not
};


// Switches back and forth from surrogate mode as defined by network model
// (e.g, by dragonfly-dally.C)
// Parameters: `data` corresponds to the lp sub-state, lp is the lp pointer, and the array of events in queue (to be processed)
typedef void (*model_switch_f) (void * data, tw_lp * lp, tw_event **);
typedef bool (*model_ask_if_freeze_f) (tw_lp * lp, tw_event * event); // Determines whether the event should be "frozen" or should be allowed to run during surrogate-mode

struct lp_types_switch {
    char lpname[MAX_NAME_LENGTH];
    bool trigger_idle_modelnet;  // Trigger idle events for model-net (prevents a model to be stuck in a schedule loop if it is to process packets during surrogate-mode). If this is true and the lpname does not start with 'modelnet_', the behaviour is undefined
    model_switch_f        highdef_to_surrogate;
    model_switch_f        surrogate_to_highdef;
    model_ask_if_freeze_f should_event_be_frozen;  // NULL means event from LP type shouldn't be frozen
    model_ask_if_freeze_f should_event_be_deleted;  // NULL means event from LP type shouldn't be deleted
};

struct switch_at_struct {
    size_t current_i;
    size_t total;
    double * time_stampts; // list of precise timestamps at which to switch
};

extern struct switch_at_struct switch_at;


// Switch
void director_call(tw_pe * pe);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
