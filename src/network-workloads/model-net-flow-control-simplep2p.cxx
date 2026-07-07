/*
 * Standalone interval-flow workload over model-net simplep2p.
 *
 * This is a normal runnable CODES binary, not a ctest harness.  It supports
 * the workflow:
 *   pure PDES record collection -> train/save flow-control ZMQML model ->
 *   hybrid full-surrogate traffic generation with PDES communication.
 */

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <ross.h>

#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"
#include "codes/model-net.h"
#include "zmqmlrequester.h"

static int net_id = 0;
static int num_servers = 0;

struct flow_feedback {
    double raw_predicted_packets;
    double available_packets;
    double granted_packets;
    double dropped_packets;
    double grant_ratio;
    double queue_packets_next;
    double queue_occupancy_ratio;
    double queue_delay_estimate;
    double capacity_packets;
    int congestion_mark_flag;
    int overflow_flag;
};

struct flow_config {
    int inferencing_enabled;
    int training_enabled;
    int debug_prints;
    double interval_seconds;
    int num_intervals;
    int packet_size_bytes;
    double bandwidth_0_to_1_mbps;
    double bandwidth_1_to_0_mbps;
    double queue_capacity_0_to_1_packets;
    double queue_capacity_1_to_0_packets;
    double base_0_to_1_packets;
    double base_1_to_0_packets;
    double burst_0_to_1_packets;
    double burst_1_to_0_packets;
    int burst_period;
    double ingress_gain;
    double mark_threshold_ratio;
    char flow_log_path[1024];
};

static flow_config cfg = {
    0,       /* inferencing_enabled */
    1,       /* training_enabled */
    0,       /* debug_prints */
    10.0,    /* interval_seconds */
    20,      /* num_intervals */
    4096,    /* packet_size_bytes */
    200.0,   /* bandwidth 0->1 */
    100.0,   /* bandwidth 1->0 */
    100000., /* queue cap 0->1 */
    100000., /* queue cap 1->0 */
    50000.,  /* pure base 0->1 */
    25000.,  /* pure base 1->0 */
    30000.,  /* burst 0->1 */
    15000.,  /* burst 1->0 */
    5,       /* burst period */
    0.10,    /* ingress gain */
    0.80,    /* mark threshold */
    ""};

struct flow_state {
    int rel_id;
    int peer_rel_id;
    tw_lpid peer_gid;
    int interval_id;
    double ingress_packets_current;
    double source_queue_packets;
    flow_feedback previous_feedback;

    double total_raw_packets;
    double total_granted_packets;
    double total_dropped_packets;
    double total_delivered_packets;

};

struct flow_msg {
    int event_type;
    int src_rel_id;
    int dst_rel_id;
    int interval_id;
    double packets;
    model_net_event_return ret;
};

enum flow_event_type {
    FLOW_INTERVAL = 1,
    FLOW_DELIVER = 2,
};

static void flow_init(flow_state* ns, tw_lp* lp);
static void flow_event(flow_state* ns, tw_bf* b, flow_msg* m, tw_lp* lp);
static void flow_rev_event(flow_state* ns, tw_bf* b, flow_msg* m, tw_lp* lp);
static void flow_finalize(flow_state* ns, tw_lp* lp);

static tw_lptype flow_lp = {
    (init_f)flow_init,
    (pre_run_f)NULL,
    (event_f)flow_event,
    (revent_f)flow_rev_event,
    (commit_f)NULL,
    (final_f)flow_finalize,
    (map_f)codes_mapping,
    sizeof(flow_state),
};

static const tw_lptype* flow_get_lp_type(void) { return &flow_lp; }

static void flow_add_lp_type(void) { lp_type_register("nw-lp", flow_get_lp_type()); }

static double seconds_to_ns(double seconds) { return seconds * 1000.0 * 1000.0 * 1000.0; }

static double ns_to_seconds(double ns) { return ns / (1000.0 * 1000.0 * 1000.0); }

static double clamp_packets(double value) {
    if (!std::isfinite(value) || value < 0.0) {
        return 0.0;
    }
    return value;
}

static flow_feedback default_feedback(void) {
    flow_feedback fb;
    std::memset(&fb, 0, sizeof(fb));
    fb.grant_ratio = 1.0;
    return fb;
}

static void read_int(const char* key, int* value) {
    int tmp = 0;
    if (configuration_get_value_int(&config, "FLOW_CONTROL", key, NULL, &tmp) == 0) {
        *value = tmp;
    }
}

static void read_double(const char* key, double* value) {
    double tmp = 0.0;
    if (configuration_get_value_double(&config, "FLOW_CONTROL", key, NULL, &tmp) == 0) {
        *value = tmp;
    }
}

static void read_string(const char* key, char* value, size_t len) {
    char tmp[1024];
    std::memset(tmp, 0, sizeof(tmp));
    if (configuration_get_value(&config, "FLOW_CONTROL", key, NULL, tmp, sizeof(tmp)) > 0) {
        std::snprintf(value, len, "%s", tmp);
    }
}

static void load_flow_config(void) {
    read_int("inferencing_enabled", &cfg.inferencing_enabled);
    read_int("training_enabled", &cfg.training_enabled);
    read_int("debug_prints", &cfg.debug_prints);
    read_double("interval_seconds", &cfg.interval_seconds);
    read_int("num_intervals", &cfg.num_intervals);
    read_int("packet_size_bytes", &cfg.packet_size_bytes);
    read_double("bandwidth_0_to_1_mbps", &cfg.bandwidth_0_to_1_mbps);
    read_double("bandwidth_1_to_0_mbps", &cfg.bandwidth_1_to_0_mbps);
    read_double("queue_capacity_0_to_1_packets", &cfg.queue_capacity_0_to_1_packets);
    read_double("queue_capacity_1_to_0_packets", &cfg.queue_capacity_1_to_0_packets);
    read_double("base_0_to_1_packets", &cfg.base_0_to_1_packets);
    read_double("base_1_to_0_packets", &cfg.base_1_to_0_packets);
    read_double("burst_0_to_1_packets", &cfg.burst_0_to_1_packets);
    read_double("burst_1_to_0_packets", &cfg.burst_1_to_0_packets);
    read_int("burst_period", &cfg.burst_period);
    read_double("ingress_gain", &cfg.ingress_gain);
    read_double("mark_threshold_ratio", &cfg.mark_threshold_ratio);
    read_string("flow_log_path", cfg.flow_log_path, sizeof(cfg.flow_log_path));

    if (cfg.interval_seconds <= 0.0) {
        tw_error(TW_LOC, "FLOW_CONTROL.interval_seconds must be positive");
    }
    if (cfg.num_intervals <= 0) {
        tw_error(TW_LOC, "FLOW_CONTROL.num_intervals must be positive");
    }
    if (cfg.packet_size_bytes <= 0) {
        tw_error(TW_LOC, "FLOW_CONTROL.packet_size_bytes must be positive");
    }
    if (cfg.burst_period <= 0) {
        cfg.burst_period = 1;
    }
}


static void push_zmqml_debug_setting(void) {
    if (!cfg.training_enabled && !cfg.inferencing_enabled) {
        return;
    }

    std::vector<std::string> args;
    args.push_back("1");
    args.push_back(cfg.debug_prints ? "1" : "0");

    std::vector<std::string> reply = zmqml_request("set-debug", args);
    if (reply.empty() || reply[0] != "done") {
        std::fprintf(stderr,
                     "[flow-control simplep2p] warning: failed to propagate debug_prints=%d to zmqmlserver\n",
                     cfg.debug_prints);
    }
}

static double bandwidth_for_flow(int src_rel, int dst_rel) {
    if (src_rel == 0 && dst_rel == 1) {
        return cfg.bandwidth_0_to_1_mbps;
    }
    if (src_rel == 1 && dst_rel == 0) {
        return cfg.bandwidth_1_to_0_mbps;
    }
    return 0.0;
}

static double queue_capacity_for_flow(int src_rel, int dst_rel) {
    if (src_rel == 0 && dst_rel == 1) {
        return cfg.queue_capacity_0_to_1_packets;
    }
    if (src_rel == 1 && dst_rel == 0) {
        return cfg.queue_capacity_1_to_0_packets;
    }
    return 0.0;
}

static double interval_capacity_packets(int src_rel, int dst_rel) {
    double bw_mbps = bandwidth_for_flow(src_rel, dst_rel);
    double bytes = bw_mbps * 1000.0 * 1000.0 / 8.0 * cfg.interval_seconds;
    return clamp_packets(std::floor(bytes / (double)cfg.packet_size_bytes));
}

static std::string flow_key(int src_rel, int dst_rel) {
    std::ostringstream os;
    os << src_rel << "->" << dst_rel;
    return os.str();
}

static std::string json_escape(const std::string& in) {
    std::ostringstream os;
    for (char c : in) {
        switch (c) {
        case '\\': os << "\\\\"; break;
        case '"': os << "\\\""; break;
        case '\n': os << "\\n"; break;
        case '\r': os << "\\r"; break;
        case '\t': os << "\\t"; break;
        default: os << c; break;
        }
    }
    return os.str();
}

static std::string build_flow_control_payload(const flow_state* ns) {
    const std::string in_key = flow_key(ns->peer_rel_id, ns->rel_id);
    const std::string out_key = flow_key(ns->rel_id, ns->peer_rel_id);
    const flow_feedback& fb = ns->previous_feedback;

    std::ostringstream os;
    os << std::setprecision(17);
    os << "{";
    os << "\"lp_id\":" << ns->rel_id << ",";
    os << "\"interval_id\":" << ns->interval_id << ",";
    os << "\"interval_seconds\":" << cfg.interval_seconds << ",";
    os << "\"incoming_flows\":{";
    os << "\"" << json_escape(in_key) << "\":{\"packets\":"
       << ns->ingress_packets_current << "}";
    os << "},";
    os << "\"outgoing_feedback\":{";
    os << "\"" << json_escape(out_key) << "\":{";
    os << "\"raw_predicted_packets\":" << fb.raw_predicted_packets << ",";
    os << "\"available_packets\":" << fb.available_packets << ",";
    os << "\"granted_packets\":" << fb.granted_packets << ",";
    os << "\"dropped_packets\":" << fb.dropped_packets << ",";
    os << "\"grant_ratio\":" << fb.grant_ratio << ",";
    os << "\"queue_packets_next\":" << fb.queue_packets_next << ",";
    os << "\"queue_occupancy_ratio\":" << fb.queue_occupancy_ratio << ",";
    os << "\"queue_delay_estimate\":" << fb.queue_delay_estimate << ",";
    os << "\"capacity_packets\":" << fb.capacity_packets << ",";
    os << "\"congestion_mark_flag\":" << fb.congestion_mark_flag << ",";
    os << "\"overflow_flag\":" << fb.overflow_flag;
    os << "}";
    os << "},";
    os << "\"outgoing_flow_keys\":[\"" << json_escape(out_key) << "\"]";
    os << "}";
    return os.str();
}

static double parse_prediction_for_flow(const std::string& predictions,
                                        const std::string& out_key,
                                        double fallback) {
    std::istringstream is(predictions);
    std::string token;
    const std::string prefix = out_key + ":";
    while (is >> token) {
        if (token.compare(0, prefix.size(), prefix) == 0) {
            char* end = NULL;
            const char* start = token.c_str() + prefix.size();
            double value = std::strtod(start, &end);
            if (end != start) {
                return clamp_packets(value);
            }
        }
    }
    return clamp_packets(fallback);
}

static double pure_pdes_raw_packets(const flow_state* ns) {
    const bool fwd = ns->rel_id == 0 && ns->peer_rel_id == 1;
    const double base = fwd ? cfg.base_0_to_1_packets : cfg.base_1_to_0_packets;
    const double burst = fwd ? cfg.burst_0_to_1_packets : cfg.burst_1_to_0_packets;
    const int phase = fwd ? 0 : 2;
    const bool burst_now = ((ns->interval_id + phase) % cfg.burst_period) == 0;
    const double ramp = 0.03 * base * (double)(ns->interval_id % cfg.burst_period);
    const double response = cfg.ingress_gain * ns->ingress_packets_current;
    return clamp_packets(base + ramp + (burst_now ? burst : 0.0) + response);
}

static flow_feedback apply_flow_control(double raw_predicted_packets, double queued_packets,
                                        int src_rel, int dst_rel) {
    flow_feedback fb = default_feedback();
    const double capacity = interval_capacity_packets(src_rel, dst_rel);
    const double queue_cap = queue_capacity_for_flow(src_rel, dst_rel);
    const double available = queued_packets + raw_predicted_packets;
    const double granted = std::min(available, capacity);
    const double unsent = std::max(0.0, available - granted);
    const double queue_next = std::min(unsent, queue_cap);
    const double dropped = std::max(0.0, unsent - queue_cap);
    const double capacity_per_second = cfg.interval_seconds > 0.0 ? capacity / cfg.interval_seconds : 0.0;

    fb.raw_predicted_packets = raw_predicted_packets;
    fb.available_packets = available;
    fb.granted_packets = granted;
    fb.dropped_packets = dropped;
    fb.grant_ratio = available > 0.0 ? granted / available : 1.0;
    fb.queue_packets_next = queue_next;
    fb.queue_occupancy_ratio = queue_cap > 0.0 ? queue_next / queue_cap : 0.0;
    fb.queue_delay_estimate = capacity_per_second > 0.0 ? queue_next / capacity_per_second : 0.0;
    fb.capacity_packets = capacity;
    fb.congestion_mark_flag = fb.queue_occupancy_ratio >= cfg.mark_threshold_ratio ? 1 : 0;
    fb.overflow_flag = dropped > 0.0 ? 1 : 0;
    return fb;
}

static void send_training_record(flow_state* ns, double raw_packets) {
    const std::string in_key = flow_key(ns->peer_rel_id, ns->rel_id);
    const std::string out_key = flow_key(ns->rel_id, ns->peer_rel_id);

    std::ostringstream record;
    record << "schema_version,lp_id,interval_id,interval_seconds,incoming_flow_key,"
           << "incoming_packets,outgoing_flow_key,raw_egress_packets,prev_grant_ratio,"
           << "prev_queue_occupancy_ratio,prev_dropped_packets,prev_capacity_packets\n";
    record << std::setprecision(17)
           << 1 << ',' << ns->rel_id << ',' << ns->interval_id << ','
           << cfg.interval_seconds << ',' << in_key << ','
           << ns->ingress_packets_current << ',' << out_key << ',' << raw_packets << ','
           << ns->previous_feedback.grant_ratio << ','
           << ns->previous_feedback.queue_occupancy_ratio << ','
           << ns->previous_feedback.dropped_packets << ','
           << ns->previous_feedback.capacity_packets << '\n';

    std::vector<std::string> reply = zmqml_director_request(
        "flow-control", "simplep2p", "send-records", std::vector<std::string>(), record.str());
    if (reply.empty() || reply[0] != "done") {
        std::fprintf(stderr,
                     "[flow-control simplep2p] warning: failed to send training record for lp %d interval %d\n",
                     ns->rel_id, ns->interval_id);
    }
}

static void append_flow_log(const flow_state* ns, const flow_feedback& fb, double raw_packets,
                            const char* source, tw_lp* lp) {
    if (cfg.flow_log_path[0] == '\0') {
        return;
    }

    const bool new_file = std::ifstream(cfg.flow_log_path).good() == false;
    std::ofstream out(cfg.flow_log_path, std::ios::app);
    if (!out) {
        if (cfg.debug_prints) {
            std::fprintf(stderr, "[flow-control simplep2p] could not write %s\n", cfg.flow_log_path);
        }
        return;
    }
    if (new_file) {
        out << "lp_gid,lp_rel,peer_rel,interval_id,sim_time_seconds,source,incoming_packets,"
            << "raw_predicted_packets,queue_before_packets,available_packets,capacity_packets,"
            << "granted_packets,queue_after_packets,dropped_packets,grant_ratio,"
            << "queue_occupancy_ratio,queue_delay_estimate,congestion_mark_flag,overflow_flag\n";
    }
    out << std::setprecision(17)
        << (unsigned long long)lp->gid << ',' << ns->rel_id << ',' << ns->peer_rel_id << ','
        << ns->interval_id << ',' << ns_to_seconds(tw_now(lp)) << ',' << source << ','
        << ns->ingress_packets_current << ',' << raw_packets << ',' << ns->source_queue_packets << ','
        << fb.available_packets << ',' << fb.capacity_packets << ',' << fb.granted_packets << ','
        << fb.queue_packets_next << ',' << fb.dropped_packets << ',' << fb.grant_ratio << ','
        << fb.queue_occupancy_ratio << ',' << fb.queue_delay_estimate << ','
        << fb.congestion_mark_flag << ',' << fb.overflow_flag << '\n';
}

static double infer_raw_packets(flow_state* ns) {
    const std::string out_key = flow_key(ns->rel_id, ns->peer_rel_id);
    const double fallback = pure_pdes_raw_packets(ns);
    const std::string payload = build_flow_control_payload(ns);

    std::vector<std::string> reply = zmqml_director_request(
        "flow-control", "simplep2p", "inference", std::vector<std::string>(), payload);

    if (reply.empty() || reply[0] != "done") {
        std::fprintf(stderr,
                     "[flow-control simplep2p] warning: inference failed for flow %s interval %d; using fallback %f\n",
                     out_key.c_str(), ns->interval_id, fallback);
        return fallback;
    }

    std::string predictions;
    if (reply.size() >= 4) {
        predictions = reply[3];
    } else if (reply.size() >= 3) {
        predictions = reply[2];
    }
    return parse_prediction_for_flow(predictions, out_key, fallback);
}

static void send_granted_packets(flow_state* ns, flow_msg* m, tw_lp* lp, double granted_packets) {
    if (granted_packets <= 0.0) {
        return;
    }

    const double bytes_double = granted_packets * (double)cfg.packet_size_bytes;
    uint64_t message_size = 0;
    if (bytes_double >= (double)std::numeric_limits<uint64_t>::max()) {
        message_size = std::numeric_limits<uint64_t>::max();
    } else {
        message_size = (uint64_t)std::ceil(bytes_double);
    }
    if (message_size == 0) {
        return;
    }

    flow_msg remote;
    std::memset(&remote, 0, sizeof(remote));
    remote.event_type = FLOW_DELIVER;
    remote.src_rel_id = ns->rel_id;
    remote.dst_rel_id = ns->peer_rel_id;
    remote.interval_id = ns->interval_id;
    remote.packets = granted_packets;

    m->ret = model_net_event(net_id, "flow-control", ns->peer_gid, message_size, 0.0,
                             sizeof(remote), &remote, 0, NULL, lp);
}

static void schedule_next_interval(flow_state* ns, tw_lp* lp) {
    if (ns->interval_id >= cfg.num_intervals) {
        return;
    }
    tw_event* e = tw_event_new(lp->gid, seconds_to_ns(cfg.interval_seconds), lp);
    flow_msg* m = (flow_msg*)tw_event_data(e);
    std::memset(m, 0, sizeof(*m));
    m->event_type = FLOW_INTERVAL;
    tw_event_send(e);
}

static void handle_interval(flow_state* ns, flow_msg* m, tw_lp* lp) {
    double raw_packets = 0.0;
    const char* source = "pure-pdes";

    if (cfg.inferencing_enabled) {
        raw_packets = infer_raw_packets(ns);
        source = "flow-control-ml";
    } else {
        raw_packets = pure_pdes_raw_packets(ns);
    }

    raw_packets = clamp_packets(raw_packets);

    if (cfg.training_enabled) {
        send_training_record(ns, raw_packets);
    }

    flow_feedback fb = apply_flow_control(raw_packets, ns->source_queue_packets,
                                          ns->rel_id, ns->peer_rel_id);
    append_flow_log(ns, fb, raw_packets, source, lp);

    if (cfg.debug_prints &&
        (fb.grant_ratio < 0.999999 || fb.dropped_packets > 0.0 || fb.congestion_mark_flag)) {
        std::printf("[flow-control throttle] lp=%llu flow=%d->%d interval=%d source=%s "
                    "raw=%.6f queue_before=%.6f available=%.6f capacity=%.6f "
                    "granted=%.6f queue_after=%.6f dropped=%.6f grant_ratio=%.6f "
                    "queue_occ=%.6f mark=%d overflow=%d\n",
                    (unsigned long long)lp->gid, ns->rel_id, ns->peer_rel_id,
                    ns->interval_id, source, raw_packets, ns->source_queue_packets,
                    fb.available_packets, fb.capacity_packets, fb.granted_packets,
                    fb.queue_packets_next, fb.dropped_packets, fb.grant_ratio,
                    fb.queue_occupancy_ratio, fb.congestion_mark_flag, fb.overflow_flag);
        std::fflush(stdout);
    }

    send_granted_packets(ns, m, lp, fb.granted_packets);

    ns->source_queue_packets = fb.queue_packets_next;
    ns->previous_feedback = fb;
    ns->total_raw_packets += raw_packets;
    ns->total_granted_packets += fb.granted_packets;
    ns->total_dropped_packets += fb.dropped_packets;
    ns->ingress_packets_current = 0.0;
    ns->interval_id++;

    schedule_next_interval(ns, lp);
}

static void handle_deliver(flow_state* ns, const flow_msg* m) {
    ns->ingress_packets_current += m->packets;
    ns->total_delivered_packets += m->packets;
}

static void flow_init(flow_state* ns, tw_lp* lp) {
    std::memset(ns, 0, sizeof(*ns));
    ns->rel_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    ns->peer_rel_id = 1 - ns->rel_id;
    ns->previous_feedback = default_feedback();

    codes_mapping_get_lp_id("MODELNET_GRP", "nw-lp", NULL, 1, ns->peer_rel_id, 0,
                            &ns->peer_gid);

    tw_event* e = tw_event_new(lp->gid, g_tw_lookahead + tw_rand_unif(lp->rng), lp);
    flow_msg* m = (flow_msg*)tw_event_data(e);
    std::memset(m, 0, sizeof(*m));
    m->event_type = FLOW_INTERVAL;
    tw_event_send(e);
}

static void flow_event(flow_state* ns, tw_bf* b, flow_msg* m, tw_lp* lp) {
    (void)b;
    switch (m->event_type) {
    case FLOW_INTERVAL:
        handle_interval(ns, m, lp);
        break;
    case FLOW_DELIVER:
        handle_deliver(ns, m);
        break;
    default:
        tw_error(TW_LOC, "unknown flow-control event type %d", m->event_type);
    }
}

static void flow_rev_event(flow_state* ns, tw_bf* b, flow_msg* m, tw_lp* lp) {
    (void)ns;
    (void)b;
    (void)m;
    (void)lp;
    tw_error(TW_LOC, "model-net-flow-control-simplep2p is intended for sequential runs first");
}

static void flow_finalize(flow_state* ns, tw_lp* lp) {
    std::printf("flow-control-simplep2p lp=%llu rel=%d peer=%d intervals=%d raw=%f granted=%f "
                "delivered=%f dropped=%f queue=%f\n",
                (unsigned long long)lp->gid, ns->rel_id, ns->peer_rel_id, ns->interval_id,
                ns->total_raw_packets, ns->total_granted_packets, ns->total_delivered_packets,
                ns->total_dropped_packets, ns->source_queue_packets);
}

const tw_optdef app_opt[] = {TWOPT_GROUP("model-net flow-control simplep2p"), TWOPT_END()};

static const char* find_config_arg(int argc, char** argv) {
    for (int i = argc - 1; i >= 1; --i) {
        if (argv[i] == NULL) {
            continue;
        }
        const char* arg = argv[i];
        const size_t len = std::strlen(arg);
        if (len >= 5 && std::strcmp(arg + len - 5, ".conf") == 0) {
            return arg;
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    int rank = 0;
    int num_nets = 0;
    int* net_ids = NULL;

    g_tw_ts_end = seconds_to_ns(60.0 * 60.0 * 24.0 * 365.0);

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    const char* config_file = find_config_arg(argc, argv);
    if (config_file == NULL) {
        std::printf("Usage: mpirun -np <n> %s --sync=1 -- <config.conf>\n", argv[0]);
        std::printf("       or: mpirun -np <n> %s --synch=1 -- <config.conf>\n", argv[0]);
        MPI_Finalize();
        return 1;
    }

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    configuration_load(config_file, MPI_COMM_WORLD, &config);
    load_flow_config();

    if (rank == 0) {
        push_zmqml_debug_setting();
    }
    MPI_Barrier(MPI_COMM_WORLD);

    flow_add_lp_type();
    model_net_register();
    codes_mapping_setup();

    net_ids = model_net_configure(&num_nets);
    assert(num_nets == 1);
    net_id = net_ids[0];
    free(net_ids);

    num_servers = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 1);
    if (num_servers != 2) {
        tw_error(TW_LOC, "model-net-flow-control-simplep2p expects exactly 2 nw-lp LPs");
    }

    if (rank == 0 && cfg.flow_log_path[0] != '\0') {
        std::remove(cfg.flow_log_path);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0 && cfg.debug_prints) {
        std::printf("flow-control config: inferencing=%d training=%d intervals=%d interval_seconds=%f\n",
                    cfg.inferencing_enabled, cfg.training_enabled, cfg.num_intervals,
                    cfg.interval_seconds);
    }

    tw_run();
    model_net_report_stats(net_id);
    tw_end();
    return 0;
}
