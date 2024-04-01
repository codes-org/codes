#include <ross.h>
#include <codes/surrogate/lp-client/base.h>
#include <codes/codes_mapping.h>
#include <codes/codes-mpi-replay.h>


struct client_surr_state {
};


static void client_surr_init(struct client_surr_state * client, tw_lp * lp) {

}


static void client_surr_event_handler(
    struct client_surr_state * client, tw_bf * bf, struct client_surr_msg * msg, tw_lp * lp) {
    msg->arrival_time = tw_now(lp);

    tw_event *e = tw_event_new(msg->src_wkld_lpid, 2076575.16 * 91, lp);
    struct nw_message* new_msg = (struct nw_message*) tw_event_data(e);
    new_msg->msg_type = SURR_SKIP_ITERATION;
    tw_event_send(e);
}


static void client_surr_event_handler_rc(
    struct client_surr_state * client, tw_bf * bf, struct client_surr_msg * msg, tw_lp * lp) {

}


static void client_surr_event_handler_commit(
    struct client_surr_state * client, tw_bf * bf, struct client_surr_msg * msg, tw_lp * lp) {
    printf("Workload %ld iteration %d ended at %f\n", msg->src_wkld_id, msg->iteration, msg->arrival_time);
}


static struct tw_lptype client_surrogate = {
    (init_f) client_surr_init,
    (pre_run_f) NULL,
    (event_f) client_surr_event_handler,
    (revent_f) client_surr_event_handler_rc,
    (commit_f) client_surr_event_handler_commit,
    (final_f) NULL,
    (map_f) codes_mapping,
    sizeof(struct client_surr_state)
};


struct tw_lptype const * client_surrogate_get_lp_type(void) {
    return(&client_surrogate);
}


void client_surrogate_register_lp_type() {
    lp_type_register("client-surrogate", client_surrogate_get_lp_type());
}
