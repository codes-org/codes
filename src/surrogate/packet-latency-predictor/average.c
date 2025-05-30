#include <codes/surrogate/init.h>
#include <codes/surrogate/packet-latency-predictor/average.h>

double ignore_until = 0;


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

    if (end->travel_end_time < ignore_until) {
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

    // TODO (Elkin): 10 is an arbitrary small value, but it should be nic_ts as implemented in `packet_getenerate` in dragonfly-dally
    double const next_packet_delay = data->aggregated_next_packet_delay.total_msgs == 0 ? 10 :
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


struct packet_latency_predictor average_latency_predictor(int num_terminals) {
    return (struct packet_latency_predictor) {
    .init              = (init_pred_lat_f) init_pred,
    .feed              = (feed_pred_lat_f) feed_pred,
    .predict           = (predict_pred_lat_f) predict_latency,
    .predict_rc        = (predict_pred_lat_rc_f) predict_latency_rc,
    .predictor_data_sz = sizeof(struct latency_surrogate) + num_terminals * sizeof(struct aggregated_latency_one_terminal)
    };
}
//
// === END OF Average packet latency functionality
