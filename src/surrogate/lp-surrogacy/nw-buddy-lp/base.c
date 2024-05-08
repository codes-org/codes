#include <ross.h>
#include <codes/surrogate/lp-surrogacy/nw-buddy-lp/base.h>
#include <codes/codes_mapping.h>
#include <codes/codes-mpi-replay.h>


struct nw_buddy_state {
};


static void nw_buddy_init(struct nw_buddy_state * buddy, tw_lp * lp) {
    // TODO: figure out the app id they are part
}


static void nw_buddy_event_handler(
    struct nw_buddy_state * buddy, tw_bf * bf, struct nw_buddy_msg * msg, tw_lp * lp) {

    if (msg->iteration == 4) {
        tw_event *e = tw_event_new(msg->src_wkld_lpid, 3440000 * 30, lp);
        struct nw_message* new_msg = (struct nw_message*) tw_event_data(e);
        new_msg->msg_type = SURR_SKIP_ITERATION;
        new_msg->fwd.skip_iterations = 30;
        tw_event_send(e);
    } else {
        tw_event *e = tw_event_new(msg->src_wkld_lpid, 0, lp);
        struct nw_message* new_msg = (struct nw_message*) tw_event_data(e);
        new_msg->msg_type = SURR_START_NEXT_ITERATION;
        tw_event_send(e);
    }
}


static void nw_buddy_event_handler_rc(
    struct nw_buddy_state * buddy, tw_bf * bf, struct nw_buddy_msg * msg, tw_lp * lp) {

}


static void nw_buddy_handler_commit(
    struct nw_buddy_state * buddy, tw_bf * bf, struct nw_buddy_msg * msg, tw_lp * lp) {
    printf("Workload %ld iteration %d ended at %f\n", msg->src_wkld_id, msg->iteration, msg->iteration_time);
}


static struct tw_lptype nw_buddy_type = {
    (init_f) nw_buddy_init,
    (pre_run_f) NULL,
    (event_f) nw_buddy_event_handler,
    (revent_f) nw_buddy_event_handler_rc,
    (commit_f) nw_buddy_handler_commit,
    (final_f) NULL,
    (map_f) codes_mapping,
    sizeof(struct nw_buddy_state)
};


struct tw_lptype const * nw_buddy_get_lp_type(void) {
    return(&nw_buddy_type);
}


void nw_buddy_surrogate_register_lp_type() {
    lp_type_register("nw-buddy-lp", nw_buddy_get_lp_type());
}
