#include "surrogate/application-surrogate.h"
#include <ross-extern.h>

static struct app_iteration_predictor * iter_predictor;
static int every_n_gvt = 1;
static enum {
    PRE_JUMP = 0,
    POST_JUMP_switched,  // Switched to surrogate-mode
    POST_JUMP_skipped,   // Did not switch, and skipping until next application finishes
} director_state;

#ifdef USE_RAND_TIEBREAKER
#define gvt_for(pe) (pe->GVT_sig.recv_ts)
#else
#define gvt_for(pe) (pe->GVT)
#endif

#define master_printf(str, ...) if (g_tw_mynode == 0) { printf(str, __VA_ARGS__); }

static void application_director_pre_switch(tw_pe * pe) {
    if (!iter_predictor->director.is_predictor_ready()) {
        return;
    }
    struct fast_forward_values jump_to = iter_predictor->director.prepare_fast_forward_jump();
    double const restarting_at = jump_to.restarting_at > gvt_for(pe) ? jump_to.restarting_at : gvt_for(pe);
    switch (jump_to.status) {
        case FAST_FORWARD_switching:
            tw_trigger_gvt_hook_at(restarting_at + 1); // + 1 to force director to run right after we have fully fast-forward
            master_printf("Triggering switch to application iteration surrogate mode at GVT %d time %f\n", g_tw_gvt_done, gvt_for(pe));
            director_state = POST_JUMP_switched;
        break;

        case FAST_FORWARD_restart:
            tw_trigger_gvt_hook_at(restarting_at + 1); // + 1 to force director to run right after we have fully fast-forward
            director_state = POST_JUMP_skipped;
        break;
    }
}

static void application_director_post_switch(tw_pe * pe) {
    tw_trigger_gvt_hook_every(every_n_gvt);
    iter_predictor->director.reset();

    if (director_state == POST_JUMP_switched) {
        master_printf("Back to full high-fidelity application iteration mode at GVT %d time %f\n", g_tw_gvt_done, gvt_for(pe));
    } else {
        master_printf("Resetting predictor at GVT %d time %f\n", g_tw_gvt_done, gvt_for(pe));
    }
    director_state = PRE_JUMP;
}

void application_director(tw_pe * pe) {
    // Director is not called if the simulation has ended
    if (gvt_for(pe) >= g_tw_ts_end) {
        return;
    }
    switch (director_state) {
        case PRE_JUMP:
            application_director_pre_switch(pe);
        break;
        case POST_JUMP_switched:
        case POST_JUMP_skipped:
            application_director_post_switch(pe);
        break;
    }
}

void application_director_configure(int every_n_gvt_, struct app_iteration_predictor * iter_predictor_) {
    every_n_gvt = every_n_gvt_;
    iter_predictor = iter_predictor_;
    g_tw_gvt_hook = application_director;
    director_state = PRE_JUMP;
    tw_trigger_gvt_hook_every(every_n_gvt);
}
