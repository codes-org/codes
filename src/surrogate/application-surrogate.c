#include "surrogate/application-surrogate.h"
#include <ross-extern.h>
#include "surrogate/network-surrogate.h"
#include "surrogate/init.h"

static struct app_iteration_predictor * iter_predictor;
static struct application_director_config conf = {
    .option = APP_DIRECTOR_OPTS_call_every_ns,
    .every_n_gvt = 1000000,
    .use_network_surrogate = false
};
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

#define master_printf(...) if (g_tw_mynode == 0) { printf(__VA_ARGS__); }

static void application_director_pre_switch(tw_pe * pe, bool is_queue_empty) {
    // No need to switch to surrogate when the simulation has ended
    if (is_queue_empty || gvt_for(pe) >= g_tw_ts_end) {
        return;
    }
    // Scheduling next GVT hook call if it is not scheduled every tw_trigger_gvt_hook_every
    if (conf.option == APP_DIRECTOR_OPTS_call_every_ns) {
        tw_trigger_gvt_hook_at(gvt_for(pe) + conf.call_every_ns);
    }

    if (!iter_predictor->director.is_predictor_ready()) {
        return;
    }
    struct fast_forward_values jump_to = iter_predictor->director.prepare_fast_forward_jump();
    double const restarting_at = jump_to.restarting_at > gvt_for(pe) ? jump_to.restarting_at : gvt_for(pe);
    switch (jump_to.status) {
        case FAST_FORWARD_switching:
            tw_trigger_gvt_hook_at(restarting_at + 1); // + 1 to force director to run right after we have fully fast-forward
            master_printf("Triggering switch to application iteration surrogate mode at GVT %d time %f\n", g_tw_gvt_done, gvt_for(pe));

            if (conf.use_network_surrogate) {
                master_printf("Switching network surrogate on\n");
                surrogate_switch_network_model(pe, is_queue_empty);
            }

            surrogate_time_last = tw_clock_read();
            director_state = POST_JUMP_switched;
        break;

        case FAST_FORWARD_restart:
            tw_trigger_gvt_hook_at(restarting_at + 1); // + 1 to force director to run right after we have fully fast-forward
            director_state = POST_JUMP_skipped;
        break;
    }
}

static void application_director_post_switch(tw_pe * pe, bool is_queue_empty) {
    // No need to restart high-fidelity simulation if network was not suspended
    if (is_queue_empty && !conf.use_network_surrogate) {
        return;
    }

    // Scheduling next GVT hook call
    if (!is_queue_empty) {
        if (conf.option == APP_DIRECTOR_OPTS_call_every_ns) {
            tw_trigger_gvt_hook_at(gvt_for(pe) + conf.call_every_ns);
        } else {
            tw_trigger_gvt_hook_every(conf.every_n_gvt);
        }
    }

    double const start = tw_clock_read();
    iter_predictor->director.reset();
    double const end = tw_clock_read();
    surrogate_switching_time += end - start;

    if (director_state == POST_JUMP_switched) {
        master_printf("Back to full high-fidelity application iteration mode at GVT %d time %f\n", g_tw_gvt_done, gvt_for(pe));

        if (conf.use_network_surrogate) {
            master_printf("Switching network surrogate off\n");
            surrogate_switch_network_model(pe, is_queue_empty);
        }

        time_in_surrogate += start - surrogate_time_last;
        surrogate_time_last = 0.0;
    } else {
        master_printf("Resetting predictor at GVT %d time %f\n", g_tw_gvt_done, gvt_for(pe));
    }
    director_state = PRE_JUMP;
}

static void application_director(tw_pe * pe, bool is_queue_empty) {
    switch (director_state) {
        case PRE_JUMP:
            application_director_pre_switch(pe, is_queue_empty);
        break;
        case POST_JUMP_switched:
        case POST_JUMP_skipped:
            application_director_post_switch(pe, is_queue_empty);
        break;
    }
}

void application_director_configure(struct application_director_config * conf_, struct app_iteration_predictor * iter_predictor_) {
    conf = *conf_;
    iter_predictor = iter_predictor_;
    g_tw_gvt_hook = application_director;
    director_state = PRE_JUMP;
    if (conf.option == APP_DIRECTOR_OPTS_every_n_gvt) {
        tw_trigger_gvt_hook_every(conf.every_n_gvt);
    } else {
        tw_trigger_gvt_hook_at(conf.call_every_ns);
    }
}

void application_director_finalize(void) {
}
