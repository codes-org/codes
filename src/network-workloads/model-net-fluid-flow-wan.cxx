
/*
 * Standalone fluid-flow WAN switch/terminal workload model.
 *
 * Terminal LPs generate stochastic bounded workload flowlets. Switch LPs route
 * flowlets, queue them per output link, enforce a shared switch-wide buffer,
 * and send per-flowlet fragments subject to interval link capacity.
 *
 * Supports sequential validation and optimistic execution. Optimistic mode uses
 * event-local reverse metadata to undo terminal generation, switch arrivals,
 * queue mutations, egress byte deltas, and RNG draws.
 */

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <ross.h>

#include "codes_config.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"

#if CODES_HAVE_ZEROMQ
#include "zmqmlrequester.h"
#endif

static const char* GROUP_NAME = "FLUID_FLOW_WAN_GRP";
static const char* TERMINAL_LP_NAME = "fluid-flow-wan-terminal-lp";
static const char* SWITCH_LP_NAME = "fluid-flow-wan-switch-lp";

static constexpr int MAX_PORTS_PER_SWITCH = 128;
/*
 * This upper bound is intentionally part of the event-message footprint: every
 * fluid_msg carries rc_allocs[MAX_RC_ALLOCATIONS] so optimistic execution can
 * reverse a switch-egress allocation without external state. Keep
 * PARAMS.message_size >= sizeof(fluid_msg), and reduce this bound only after
 * measuring the maximum allocations per switch-egress event for the target
 * topology/workload.
 */
static constexpr int MAX_RC_ALLOCATIONS = 256;
static constexpr int MAX_ML_CANDIDATES = 32;
static constexpr double EPS = 1e-9;

static constexpr double PHASE_GENERATE = 0.10;
static constexpr double PHASE_ARRIVAL = 0.20;
static constexpr double PHASE_SWITCH_EGRESS = 0.60;

struct link_info {
    int dst_switch;
    double bandwidth_mbps;
    double buffer_mbit;
};

struct switch_info {
    std::string name;
    int terminal_count = 0;
    int terminal_start = 0;
    double terminal_bandwidth_mbps = 10.0;
    double switch_buffer_mbit = 1024.0;
    std::vector<link_info> links;
};

struct terminal_info {
    int switch_id = -1;
    int local_id = -1;
    std::string name;
};

struct sim_config {
    double interval_seconds = 1.0;
    int num_send_intervals = 20;
    int num_drain_intervals = 20;
    int rng_seed = 12345;
    int terminal_send_every_n_intervals = 1;
    double terminal_send_probability = 1.0;
    double terminal_min_send_mbit = 0.0;
    double terminal_max_send_fraction_of_link_capacity = 1.0;
    int debug_prints = 0;
    char topology_yaml_file[1024] = "";
    char terminal_log_path[1024] = "";
    char switch_log_path[1024] = "";
    char flowlet_log_path[1024] = "";
    char switch_training_log_path[1024] = "";
    char switch_scheduler[64] = "round_robin";
    int round_robin_max_entries_per_egress = 32;
    double round_robin_quantum_mbit = 0.0;

    char director_fallback_scheduler[64] = "round_robin";
    char director_candidate_selection_policy[64] = "fifo";
    int director_max_candidate_flowlets = MAX_ML_CANDIDATES;
    int director_inference_timeout_ms = 5000;
    char director_endpoint[256] = "tcp://localhost:5555";
};

static sim_config cfg;
static std::vector<switch_info> switches;
static std::vector<terminal_info> terminals;
static std::map<std::string, int> switch_name_to_id;
static std::vector<std::vector<int>> next_switch_table;
static int total_switch_lps = 0;
static int total_terminal_lps = 0;

struct queued_flowlet {
    int valid;
    unsigned long long flowlet_id;
    int source_terminal;
    int destination_terminal;
    int creation_interval;
    int enqueue_interval;
    int age_intervals;
    double remaining_mbit;
};

struct port_desc {
    int is_terminal;
    int target_index;
    double capacity_mbit_per_interval;
};

struct terminal_state {
    int terminal_id;
    int attached_switch;
    int local_terminal_id;
    unsigned long long next_flowlet_seq;
    double generated_mbit;
    double sent_to_switch_mbit;
    double received_mbit;
    int generated_flowlets;
    int received_fragments;
};

struct switch_state {
    int switch_id;
    int num_ports;
    double shared_buffer_mbit;
    port_desc ports[MAX_PORTS_PER_SWITCH];
    std::vector<queued_flowlet>* queues[MAX_PORTS_PER_SWITCH];

    /*
     * Demand-driven egress scheduling.  Each port may have at most one
     * outstanding SWITCH_EGRESS event.  -1 means no egress event is currently
     * outstanding for that port.
     */
    int scheduled_egress_interval[MAX_PORTS_PER_SWITCH];
    int rr_next_queue_index[MAX_PORTS_PER_SWITCH];

    /* Current and previous interval traffic observations used by the ML policy. */
    int traffic_metrics_interval;
    double ingress_mbit_current;
    double ingress_mbit_previous;
    double egress_mbit_current;
    double egress_mbit_previous;
    double dropped_mbit_current;
    double dropped_mbit_previous;

    double enqueued_mbit;
    double sent_mbit;
    double delivered_local_mbit;
    double dropped_mbit;
    int received_fragments;
    int sent_fragments;
};

enum fluid_event_type {
    WORKLOAD_GENERATE = 1,
    FLOWLET_ARRIVAL = 2,
    SWITCH_EGRESS = 3,
};

struct rc_alloc_record {
    int valid;
    int queue_index;
    queued_flowlet before;
    double send_mbit;
};

struct ml_candidate_record {
    int valid;
    int queue_index;
    int candidate_rank;
    queued_flowlet before;
    double allocation_mbit;
};

struct fluid_msg {
    int event_type;
    int interval_id;
    int source_terminal;
    int destination_terminal;
    int source_switch;
    int destination_switch;
    int port_id;
    int creation_interval;
    unsigned long long flowlet_id;
    double mbit;

    /*
     * Reverse-computation metadata. These fields are written by the forward
     * event handler and consumed by the reverse handler if the event rolls back.
     */
    int rc_rng_count;
    int rc_generated;
    int rc_no_route;
    int rc_port_id;
    int rc_queue_index;
    int rc_coalesced;
    double rc_accepted_mbit;
    double rc_dropped_mbit;
    int rc_alloc_count;

    int rc_prev_scheduled_egress_interval;
    int rc_prev_rr_next_queue_index;

    int rc_prev_traffic_metrics_interval;
    double rc_prev_ingress_mbit_current;
    double rc_prev_ingress_mbit_previous;
    double rc_prev_egress_mbit_current;
    double rc_prev_egress_mbit_previous;
    double rc_prev_dropped_mbit_current;
    double rc_prev_dropped_mbit_previous;

    int rc_log_target_is_terminal;
    int rc_log_target_index;
    double rc_log_capacity_mbit;
    double rc_log_port_queued_before_mbit;
    double rc_log_port_queued_after_mbit;
    double rc_log_shared_queued_before_mbit;
    double rc_log_shared_queued_after_mbit;
    double rc_log_flowlet_remaining_after_mbit;
    double rc_log_sent_total_mbit;
    int rc_log_active_before_entries;
    int rc_log_active_after_entries;

    int rc_ml_candidate_count;
    int rc_ml_total_queued_flowlets;
    int rc_ml_total_active_flowlets;
    int rc_ml_used_fallback;
    double rc_ml_ingress_mbit_current;
    double rc_ml_ingress_mbit_previous;
    double rc_ml_egress_mbit_current;
    double rc_ml_egress_mbit_previous;
    double rc_ml_dropped_mbit_current;
    double rc_ml_dropped_mbit_previous;

    rc_alloc_record rc_allocs[MAX_RC_ALLOCATIONS];
    ml_candidate_record rc_ml_candidates[MAX_ML_CANDIDATES];
};

static void terminal_init(terminal_state* ns, tw_lp* lp);
static void terminal_event(terminal_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp);
static void terminal_rev_event(terminal_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp);
static void terminal_commit_event(terminal_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp);
static void terminal_finalize(terminal_state* ns, tw_lp* lp);

static void switch_init(switch_state* ns, tw_lp* lp);
static void switch_event(switch_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp);
static void switch_rev_event(switch_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp);
static void switch_commit_event(switch_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp);
static void switch_finalize(switch_state* ns, tw_lp* lp);

static tw_lptype terminal_lp = {
    (init_f)terminal_init,
    (pre_run_f)NULL,
    (event_f)terminal_event,
    (revent_f)terminal_rev_event,
    (commit_f)terminal_commit_event,
    (final_f)terminal_finalize,
    (map_f)codes_mapping,
    sizeof(terminal_state),
};

static tw_lptype switch_lp = {
    (init_f)switch_init,           (pre_run_f)NULL,
    (event_f)switch_event,         (revent_f)switch_rev_event,
    (commit_f)switch_commit_event, (final_f)switch_finalize,
    (map_f)codes_mapping,          sizeof(switch_state),
};

static const tw_lptype* terminal_get_lp_type(void) {
    return &terminal_lp;
}
static const tw_lptype* switch_get_lp_type(void) {
    return &switch_lp;
}

static void add_lp_types(void) {
    lp_type_register(TERMINAL_LP_NAME, terminal_get_lp_type());
    lp_type_register(SWITCH_LP_NAME, switch_get_lp_type());
}

static int get_configured_message_size_bytes(void) {
    int configured_message_size = 0;
    if (configuration_get_value_int(&config, "PARAMS", "message_size", NULL,
                                    &configured_message_size) != 0) {
        return 0;
    }
    return configured_message_size;
}

static void validate_ross_message_size_or_abort(int rank) {
    int configured_message_size = get_configured_message_size_bytes();
    size_t required_message_size = sizeof(fluid_msg);

    if (configured_message_size <= 0 || (size_t)configured_message_size < required_message_size) {
        if (rank == 0) {
            fprintf(stderr,
                    "fluid-flow-wan error: PARAMS.message_size=%d is too small for "
                    "sizeof(fluid_msg)=%zu. Increase PARAMS.message_size in the "
                    "CODES config before calling codes_mapping_setup(). "
                    "fluid_msg is large because MAX_RC_ALLOCATIONS=%d reverse "
                    "allocation records are carried in each event.\n",
                    configured_message_size, required_message_size, MAX_RC_ALLOCATIONS);
        }
        MPI_Abort(MPI_COMM_CODES, 1);
    }
}

static double seconds_to_ns(double seconds) {
    return seconds * 1000.0 * 1000.0 * 1000.0;
}

static double event_time_ns(int interval_id, double phase_seconds) {
    return seconds_to_ns(((double)interval_id * cfg.interval_seconds) + phase_seconds);
}

static double delay_until_ns(int target_interval, double phase_seconds, tw_lp* lp) {
    double target = event_time_ns(target_interval, phase_seconds);
    double delta = target - tw_now(lp);
    if (delta <= g_tw_lookahead) {
        delta = g_tw_lookahead + 1.0;
    }
    return delta;
}

static std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b])) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && std::isspace((unsigned char)s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

static int leading_spaces(const std::string& s) {
    int n = 0;
    while (n < (int)s.size() && s[n] == ' ') {
        ++n;
    }
    return n;
}

static std::string strip_quotes(std::string s) {
    s = trim(s);
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

static std::string remove_inline_comment(const std::string& s) {
    bool in_quote = false;
    char quote_char = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((s[i] == '"' || s[i] == '\'') && (i == 0 || s[i - 1] != '\\')) {
            if (!in_quote) {
                in_quote = true;
                quote_char = s[i];
            } else if (quote_char == s[i]) {
                in_quote = false;
                quote_char = 0;
            }
        }
        if (!in_quote && s[i] == '#') {
            return s.substr(0, i);
        }
    }
    return s;
}

static bool split_key_value(const std::string& line, std::string* key, std::string* value) {
    size_t pos = line.find(':');
    if (pos == std::string::npos) {
        return false;
    }
    *key = trim(line.substr(0, pos));
    *value = trim(line.substr(pos + 1));
    return !key->empty();
}

static double parse_mbps(const std::string& raw) {
    std::string s = strip_quotes(raw);
    std::stringstream ss(s);
    double v = 0.0;
    ss >> v;
    if (!std::isfinite(v) || v < 0.0) {
        return 0.0;
    }
    return v;
}

static double parse_mbit(const std::string& raw) {
    std::string s = strip_quotes(raw);
    std::stringstream ss(s);
    double v = 0.0;
    ss >> v;
    if (!std::isfinite(v) || v < 0.0) {
        return 0.0;
    }
    if (s.find("GB") != std::string::npos || s.find("Gb") != std::string::npos) {
        return v * 1000.0;
    }
    if (s.find("MB") != std::string::npos || s.find("Mb") != std::string::npos) {
        return v;
    }
    return v;
}

static int get_or_add_switch(const std::string& name) {
    std::map<std::string, int>::iterator it = switch_name_to_id.find(name);
    if (it != switch_name_to_id.end()) {
        return it->second;
    }
    int id = (int)switches.size();
    switch_info sw;
    sw.name = name;
    switches.push_back(sw);
    switch_name_to_id[name] = id;
    return id;
}

static void add_or_update_link(int src, int dst, double bw_mbps, double buffer_mbit) {
    for (size_t i = 0; i < switches[src].links.size(); ++i) {
        if (switches[src].links[i].dst_switch == dst) {
            switches[src].links[i].bandwidth_mbps = bw_mbps;
            if (buffer_mbit > 0.0) {
                switches[src].links[i].buffer_mbit = buffer_mbit;
            }
            return;
        }
    }
    link_info li;
    li.dst_switch = dst;
    li.bandwidth_mbps = bw_mbps;
    li.buffer_mbit = buffer_mbit > 0.0 ? buffer_mbit : switches[src].switch_buffer_mbit;
    switches[src].links.push_back(li);
}

static void load_topology_yaml(const char* path) {
    switches.clear();
    terminals.clear();
    switch_name_to_id.clear();

    std::ifstream in(path);
    if (!in.good()) {
        tw_error(TW_LOC, "could not open topology YAML file: %s", path);
    }

    std::string section;
    bool in_switches = false;
    bool in_connections = false;
    int current_switch = -1;
    int current_conn_dst = -1;
    double current_conn_buffer = 0.0;

    std::string raw;
    while (std::getline(in, raw)) {
        std::string no_comment = remove_inline_comment(raw);
        if (trim(no_comment).empty()) {
            continue;
        }
        int indent = leading_spaces(no_comment);
        std::string line = trim(no_comment);
        if (line == "topology:") {
            section = "topology";
            in_switches = false;
            in_connections = false;
            current_switch = -1;
            continue;
        }
        if (section == "topology" && line == "switches:") {
            in_switches = true;
            in_connections = false;
            current_switch = -1;
            continue;
        }

        std::string key;
        std::string value;
        if (!split_key_value(line, &key, &value)) {
            continue;
        }

        if (section == "topology" && in_switches) {
            if (indent == 4 && value.empty()) {
                current_switch = get_or_add_switch(key);
                in_connections = false;
                current_conn_dst = -1;
                continue;
            }
            if (current_switch < 0) {
                continue;
            }
            if (indent == 6 && key == "connections") {
                in_connections = true;
                current_conn_dst = -1;
                current_conn_buffer = switches[current_switch].switch_buffer_mbit;
                continue;
            }
            if (!in_connections && indent >= 6) {
                if (key == "terminals")
                    switches[current_switch].terminal_count = atoi(strip_quotes(value).c_str());
                else if (key == "terminal_bandwidth")
                    switches[current_switch].terminal_bandwidth_mbps = parse_mbps(value);
                else if (key == "switch_buffer")
                    switches[current_switch].switch_buffer_mbit = parse_mbit(value);
                continue;
            }
            if (in_connections && indent == 8) {
                int dst = get_or_add_switch(key);
                current_conn_dst = dst;
                current_conn_buffer = switches[current_switch].switch_buffer_mbit;
                if (!value.empty()) {
                    add_or_update_link(current_switch, dst, parse_mbps(value), current_conn_buffer);
                }
                continue;
            }
            if (in_connections && indent >= 10 && current_conn_dst >= 0) {
                if (key == "bandwidth") {
                    double bw = parse_mbps(value);
                    add_or_update_link(current_switch, current_conn_dst, bw, current_conn_buffer);
                } else if (key == "buffer") {
                    current_conn_buffer = parse_mbit(value);
                    for (size_t i = 0; i < switches[current_switch].links.size(); ++i) {
                        if (switches[current_switch].links[i].dst_switch == current_conn_dst) {
                            switches[current_switch].links[i].buffer_mbit = current_conn_buffer;
                        }
                    }
                }
            }
        }
    }

    int terminal_start = 0;
    for (int s = 0; s < (int)switches.size(); ++s) {
        switches[s].terminal_start = terminal_start;
        for (int t = 0; t < switches[s].terminal_count; ++t) {
            terminal_info ti;
            ti.switch_id = s;
            ti.local_id = t;
            std::ostringstream name;
            name << switches[s].name << "." << t;
            ti.name = name.str();
            terminals.push_back(ti);
            ++terminal_start;
        }
    }

    if (switches.empty()) {
        tw_error(TW_LOC, "topology YAML defined no switches");
    }
    if (terminals.size() < 2) {
        tw_error(TW_LOC, "topology YAML must define at least two terminals");
    }
    if (cfg.interval_seconds <= 0.0)
        tw_error(TW_LOC, "interval_seconds must be positive");
    if (cfg.num_send_intervals <= 0)
        tw_error(TW_LOC, "num_send_intervals must be positive");
    if (cfg.num_drain_intervals < 0)
        cfg.num_drain_intervals = 0;
    if (cfg.terminal_send_every_n_intervals <= 0)
        cfg.terminal_send_every_n_intervals = 1;
    if (cfg.terminal_send_probability < 0.0)
        cfg.terminal_send_probability = 0.0;
    if (cfg.terminal_send_probability > 1.0)
        cfg.terminal_send_probability = 1.0;
    if (cfg.terminal_max_send_fraction_of_link_capacity < 0.0)
        cfg.terminal_max_send_fraction_of_link_capacity = 0.0;
    if (cfg.terminal_max_send_fraction_of_link_capacity > 1.0)
        cfg.terminal_max_send_fraction_of_link_capacity = 1.0;
}

static void compute_routes(void) {
    int n = (int)switches.size();
    next_switch_table.assign(n, std::vector<int>(n, -1));
    for (int src = 0; src < n; ++src) {
        std::vector<int> prev(n, -1);
        std::vector<int> seen(n, 0);
        std::queue<int> q;
        seen[src] = 1;
        q.push(src);
        while (!q.empty()) {
            int u = q.front();
            q.pop();
            for (size_t i = 0; i < switches[u].links.size(); ++i) {
                int v = switches[u].links[i].dst_switch;
                if (!seen[v]) {
                    seen[v] = 1;
                    prev[v] = u;
                    q.push(v);
                }
            }
        }
        for (int dst = 0; dst < n; ++dst) {
            if (dst == src) {
                next_switch_table[src][dst] = src;
                continue;
            }
            if (!seen[dst]) {
                next_switch_table[src][dst] = -1;
                continue;
            }
            int cur = dst;
            int parent = prev[cur];
            while (parent >= 0 && parent != src) {
                cur = parent;
                parent = prev[cur];
            }
            next_switch_table[src][dst] = cur;
        }
    }
}

static void read_int_param(const char* section, const char* key, int* value) {
    int tmp = 0;
    if (configuration_get_value_int(&config, section, key, NULL, &tmp) == 0) {
        *value = tmp;
    }
}

static void read_double_param(const char* section, const char* key, double* value) {
    double tmp = 0.0;
    if (configuration_get_value_double(&config, section, key, NULL, &tmp) == 0) {
        *value = tmp;
    }
}

static void read_string_param(const char* section, const char* key, char* value, size_t len) {
    char tmp[1024];
    memset(tmp, 0, sizeof(tmp));
    if (configuration_get_value(&config, section, key, NULL, tmp, sizeof(tmp)) > 0) {
        snprintf(value, len, "%s", tmp);
    }
}

static void read_relpath_param(const char* section, const char* key, char* value, size_t len) {
    char tmp[1024];
    memset(tmp, 0, sizeof(tmp));
    if (configuration_get_value_relpath(&config, section, key, NULL, tmp, sizeof(tmp)) > 0) {
        snprintf(value, len, "%s", tmp);
    }
}

static void load_config(void) {
    read_relpath_param("FLUID_FLOW_WAN", "topology_yaml_file", cfg.topology_yaml_file,
                       sizeof(cfg.topology_yaml_file));
    read_relpath_param("FLUID_FLOW_WAN", "terminal_log_path", cfg.terminal_log_path,
                       sizeof(cfg.terminal_log_path));
    read_relpath_param("FLUID_FLOW_WAN", "switch_log_path", cfg.switch_log_path,
                       sizeof(cfg.switch_log_path));
    read_relpath_param("FLUID_FLOW_WAN", "flowlet_log_path", cfg.flowlet_log_path,
                       sizeof(cfg.flowlet_log_path));
    read_relpath_param("FLUID_FLOW_WAN", "switch_training_log_path", cfg.switch_training_log_path,
                       sizeof(cfg.switch_training_log_path));
    read_string_param("FLUID_FLOW_WAN", "switch_scheduler", cfg.switch_scheduler,
                      sizeof(cfg.switch_scheduler));
    read_int_param("FLUID_FLOW_WAN", "round_robin_max_entries_per_egress",
                   &cfg.round_robin_max_entries_per_egress);
    read_double_param("FLUID_FLOW_WAN", "round_robin_quantum_mbit", &cfg.round_robin_quantum_mbit);
    read_string_param("FLUID_FLOW_WAN", "director_fallback_scheduler", cfg.director_fallback_scheduler,
                      sizeof(cfg.director_fallback_scheduler));
    read_string_param("FLUID_FLOW_WAN", "director_candidate_selection_policy",
                      cfg.director_candidate_selection_policy,
                      sizeof(cfg.director_candidate_selection_policy));
    read_int_param("FLUID_FLOW_WAN", "director_max_candidate_flowlets",
                   &cfg.director_max_candidate_flowlets);
    read_int_param("FLUID_FLOW_WAN", "director_inference_timeout_ms",
                   &cfg.director_inference_timeout_ms);
    read_string_param("FLUID_FLOW_WAN", "director_endpoint", cfg.director_endpoint,
                      sizeof(cfg.director_endpoint));
    read_double_param("FLUID_FLOW_WAN", "interval_seconds", &cfg.interval_seconds);
    read_int_param("FLUID_FLOW_WAN", "num_send_intervals", &cfg.num_send_intervals);
    read_int_param("FLUID_FLOW_WAN", "num_drain_intervals", &cfg.num_drain_intervals);
    read_int_param("FLUID_FLOW_WAN", "rng_seed", &cfg.rng_seed);
    read_int_param("FLUID_FLOW_WAN", "terminal_send_every_n_intervals",
                   &cfg.terminal_send_every_n_intervals);
    read_double_param("FLUID_FLOW_WAN", "terminal_send_probability",
                      &cfg.terminal_send_probability);
    read_double_param("FLUID_FLOW_WAN", "terminal_min_send_mbit", &cfg.terminal_min_send_mbit);
    read_double_param("FLUID_FLOW_WAN", "terminal_max_send_fraction_of_link_capacity",
                      &cfg.terminal_max_send_fraction_of_link_capacity);
    read_int_param("FLUID_FLOW_WAN", "debug_prints", &cfg.debug_prints);

    if (strcmp(cfg.switch_scheduler, "fifo") != 0 &&
        strcmp(cfg.switch_scheduler, "round_robin") != 0 &&
        strcmp(cfg.switch_scheduler, "ml") != 0) {
        tw_error(TW_LOC,
                 "FLUID_FLOW_WAN.switch_scheduler must be one of: fifo, round_robin, ml; got '%s'",
                 cfg.switch_scheduler);
    }

#if !CODES_HAVE_ZEROMQ
    if (strcmp(cfg.switch_scheduler, "ml") == 0) {
        tw_error(TW_LOC,
                 "FLUID_FLOW_WAN.switch_scheduler=ml requires a build configured with USE_ZEROMQ");
    }
#endif

    if (strcmp(cfg.director_fallback_scheduler, "fifo") != 0 &&
        strcmp(cfg.director_fallback_scheduler, "round_robin") != 0) {
        tw_error(TW_LOC,
                 "FLUID_FLOW_WAN.director_fallback_scheduler must be fifo or round_robin; got '%s'",
                 cfg.director_fallback_scheduler);
    }

    if (strcmp(cfg.director_candidate_selection_policy, "fifo") != 0 &&
        strcmp(cfg.director_candidate_selection_policy, "sjf") != 0 &&
        strcmp(cfg.director_candidate_selection_policy, "ljf") != 0) {
        tw_error(TW_LOC,
                 "FLUID_FLOW_WAN.director_candidate_selection_policy must be fifo, sjf, or ljf; got '%s'",
                 cfg.director_candidate_selection_policy);
    }

    if (cfg.director_max_candidate_flowlets <= 0 ||
        cfg.director_max_candidate_flowlets > MAX_ML_CANDIDATES) {
        tw_error(TW_LOC,
                 "FLUID_FLOW_WAN.director_max_candidate_flowlets must be in [1, %d]; got %d",
                 MAX_ML_CANDIDATES, cfg.director_max_candidate_flowlets);
    }

    if (cfg.director_inference_timeout_ms <= 0) {
        tw_error(TW_LOC,
                 "FLUID_FLOW_WAN.director_inference_timeout_ms must be positive; got %d",
                 cfg.director_inference_timeout_ms);
    }

    if (strcmp(cfg.switch_scheduler, "ml") == 0 && cfg.director_endpoint[0] == '\0') {
        tw_error(TW_LOC,
                 "FLUID_FLOW_WAN.director_endpoint is required when switch_scheduler=ml");
    }

    if (cfg.round_robin_max_entries_per_egress <= 0) {
        tw_error(TW_LOC,
                 "FLUID_FLOW_WAN.round_robin_max_entries_per_egress must be positive; got %d",
                 cfg.round_robin_max_entries_per_egress);
    }

    if (cfg.round_robin_max_entries_per_egress > MAX_RC_ALLOCATIONS) {
        tw_error(TW_LOC,
                 "FLUID_FLOW_WAN.round_robin_max_entries_per_egress=%d exceeds "
                 "MAX_RC_ALLOCATIONS=%d",
                 cfg.round_robin_max_entries_per_egress, MAX_RC_ALLOCATIONS);
    }

    if (cfg.round_robin_quantum_mbit < 0.0) {
        tw_error(TW_LOC, "FLUID_FLOW_WAN.round_robin_quantum_mbit must be nonnegative; got %.12f",
                 cfg.round_robin_quantum_mbit);
    }

    if (cfg.topology_yaml_file[0] == '\0') {
        tw_error(TW_LOC, "FLUID_FLOW_WAN.topology_yaml_file is required");
    }
    load_topology_yaml(cfg.topology_yaml_file);
    compute_routes();
}

static void configure_fluid_flow_wan_server_debug(int rank) {
#if CODES_HAVE_ZEROMQ
    if (rank != 0 || strcmp(cfg.switch_scheduler, "ml") != 0) {
        return;
    }

    try {
        const std::vector<std::string> reply = zmqml_director_request(
            "fluid-flow-wan", "torchscript", "set-debug",
            {"1", cfg.debug_prints ? "1" : "0"}, "None",
            cfg.director_inference_timeout_ms, cfg.director_endpoint);

        if ((reply.empty() || reply[0] != "done") && cfg.debug_prints) {
            fprintf(stderr,
                    "[fluid-flow-wan ML] could not synchronize server "
                    "debug_prints=%d\n",
                    cfg.debug_prints);
        }
    } catch (const std::exception& ex) {
        /*
         * Keep startup best-effort so the configured deterministic fallback
         * can still handle an unavailable inference server.
         */
        if (cfg.debug_prints) {
            fprintf(stderr,
                    "[fluid-flow-wan ML] could not synchronize server "
                    "debug_prints=%d: %s\n",
                    cfg.debug_prints, ex.what());
        }
    }
#else
    (void)rank;
#endif
}

static tw_lpid get_switch_gid(int switch_id) {
    if (switch_id < 0 || switch_id >= (int)switches.size()) {
        tw_error(TW_LOC, "switch id %d out of range [0, %zu)", switch_id, switches.size());
    }

    tw_lpid gid = 0;

    /*
     * LPGROUPS has one FLUID_FLOW_WAN_GRP repetition containing all switch LPs.
     * The switch index is therefore the LP-type offset, not the group repetition.
     */
    codes_mapping_get_lp_id(GROUP_NAME, SWITCH_LP_NAME, NULL, 1, 0, switch_id, &gid);

    return gid;
}

static tw_lpid get_terminal_gid(int terminal_id) {
    if (terminal_id < 0 || terminal_id >= (int)terminals.size()) {
        tw_error(TW_LOC, "terminal id %d out of range [0, %zu)", terminal_id, terminals.size());
    }

    tw_lpid gid = 0;

    /*
     * LPGROUPS has one FLUID_FLOW_WAN_GRP repetition containing all terminal LPs.
     * The terminal index is therefore the LP-type offset, not the group repetition.
     */
    codes_mapping_get_lp_id(GROUP_NAME, TERMINAL_LP_NAME, NULL, 1, 0, terminal_id, &gid);

    return gid;
}

static int find_switch_link_port(const switch_state* ns, int dst_switch) {
    for (int p = 0; p < ns->num_ports; ++p) {
        if (!ns->ports[p].is_terminal && ns->ports[p].target_index == dst_switch) {
            return p;
        }
    }
    return -1;
}

static int find_terminal_port(const switch_state* ns, int dst_terminal) {
    for (int p = 0; p < ns->num_ports; ++p) {
        if (ns->ports[p].is_terminal && ns->ports[p].target_index == dst_terminal) {
            return p;
        }
    }
    return -1;
}

static double queued_mbit_on_switch(const switch_state* ns);

static double enqueue_flowlet(switch_state* ns, int port_id, const fluid_msg* m,
                              double* port_queued_before_out, double* shared_queued_before_out,
                              double* shared_queued_after_out, double* dropped_out,
                              double* flowlet_remaining_after_out, int* coalesced_out,
                              int* queue_index_out) {
    if (port_queued_before_out != NULL) {
        *port_queued_before_out = 0.0;
    }
    if (shared_queued_before_out != NULL) {
        *shared_queued_before_out = 0.0;
    }
    if (shared_queued_after_out != NULL) {
        *shared_queued_after_out = 0.0;
    }
    if (dropped_out != NULL) {
        *dropped_out = 0.0;
    }
    if (flowlet_remaining_after_out != NULL) {
        *flowlet_remaining_after_out = 0.0;
    }
    if (coalesced_out != NULL) {
        *coalesced_out = 0;
    }
    if (queue_index_out != NULL) {
        *queue_index_out = -1;
    }

    if (port_id < 0 || port_id >= ns->num_ports || ns->queues[port_id] == NULL) {
        if (dropped_out != NULL) {
            *dropped_out = m->mbit;
        }
        return 0.0;
    }

    std::vector<queued_flowlet>& qv = *ns->queues[port_id];

    double port_queued_before = 0.0;
    for (size_t i = 0; i < qv.size(); ++i) {
        port_queued_before += qv[i].remaining_mbit;
    }

    double shared_queued_before = queued_mbit_on_switch(ns);

    if (port_queued_before_out != NULL) {
        *port_queued_before_out = port_queued_before;
    }
    if (shared_queued_before_out != NULL) {
        *shared_queued_before_out = shared_queued_before;
    }

    /*
     * Admission is controlled by the shared switch-wide buffer. Output queues
     * remain per port, but they all draw from this one occupancy limit.
     */
    double shared_available = std::max(0.0, ns->shared_buffer_mbit - shared_queued_before);
    double accepted = std::min(m->mbit, shared_available);
    double dropped = std::max(0.0, m->mbit - accepted);

    if (dropped > EPS) {
        ns->dropped_mbit += dropped;
    }

    if (dropped_out != NULL) {
        *dropped_out = dropped;
    }

    if (accepted <= EPS) {
        if (shared_queued_after_out != NULL) {
            *shared_queued_after_out = shared_queued_before;
        }
        return 0.0;
    }

    for (size_t i = 0; i < qv.size(); ++i) {
        if (!qv[i].valid) {
            continue;
        }

        if (qv[i].flowlet_id == m->flowlet_id && qv[i].source_terminal == m->source_terminal &&
            qv[i].destination_terminal == m->destination_terminal &&
            qv[i].creation_interval == m->creation_interval) {
            qv[i].remaining_mbit += accepted;
            qv[i].age_intervals = m->interval_id - m->creation_interval;
            ns->enqueued_mbit += accepted;

            if (flowlet_remaining_after_out != NULL) {
                *flowlet_remaining_after_out = qv[i].remaining_mbit;
            }
            if (coalesced_out != NULL) {
                *coalesced_out = 1;
            }
            if (queue_index_out != NULL) {
                *queue_index_out = (int)i;
            }
            if (shared_queued_after_out != NULL) {
                *shared_queued_after_out = shared_queued_before + accepted;
            }

            return accepted;
        }
    }

    queued_flowlet q;
    memset(&q, 0, sizeof(q));
    q.valid = 1;
    q.flowlet_id = m->flowlet_id;
    q.source_terminal = m->source_terminal;
    q.destination_terminal = m->destination_terminal;
    q.creation_interval = m->creation_interval;
    q.enqueue_interval = m->interval_id;
    q.age_intervals = m->interval_id - m->creation_interval;
    q.remaining_mbit = accepted;

    qv.push_back(q);
    if (queue_index_out != NULL) {
        *queue_index_out = (int)qv.size() - 1;
    }
    ns->enqueued_mbit += accepted;

    if (flowlet_remaining_after_out != NULL) {
        *flowlet_remaining_after_out = accepted;
    }
    if (shared_queued_after_out != NULL) {
        *shared_queued_after_out = shared_queued_before + accepted;
    }

    return accepted;
}


static double queued_mbit_on_port(const switch_state* ns, int port_id) {
    if (port_id < 0 || port_id >= ns->num_ports || ns->queues[port_id] == NULL) {
        return 0.0;
    }
    double total = 0.0;
    const std::vector<queued_flowlet>& qv = *ns->queues[port_id];
    for (size_t i = 0; i < qv.size(); ++i) {
        total += qv[i].remaining_mbit;
    }
    return total;
}

static double queued_mbit_on_switch(const switch_state* ns) {
    double total = 0.0;
    for (int p = 0; p < ns->num_ports; ++p) {
        total += queued_mbit_on_port(ns, p);
    }
    return total;
}


static int active_flowlet_count_on_port(const switch_state* ns, int port_id) {
    if (port_id < 0 || port_id >= ns->num_ports || ns->queues[port_id] == NULL) {
        return 0;
    }
    int count = 0;
    const std::vector<queued_flowlet>& qv = *ns->queues[port_id];
    for (size_t i = 0; i < qv.size(); ++i) {
        if (qv[i].valid && qv[i].remaining_mbit > EPS) {
            ++count;
        }
    }
    return count;
}


static int active_flowlet_count_on_switch(const switch_state* ns) {
    int count = 0;
    for (int port_id = 0; port_id < ns->num_ports; ++port_id) {
        count += active_flowlet_count_on_port(ns, port_id);
    }
    return count;
}

static void begin_switch_traffic_metrics_event(switch_state* ns, fluid_msg* m) {
    m->rc_prev_traffic_metrics_interval = ns->traffic_metrics_interval;
    m->rc_prev_ingress_mbit_current = ns->ingress_mbit_current;
    m->rc_prev_ingress_mbit_previous = ns->ingress_mbit_previous;
    m->rc_prev_egress_mbit_current = ns->egress_mbit_current;
    m->rc_prev_egress_mbit_previous = ns->egress_mbit_previous;
    m->rc_prev_dropped_mbit_current = ns->dropped_mbit_current;
    m->rc_prev_dropped_mbit_previous = ns->dropped_mbit_previous;

    if (ns->traffic_metrics_interval == m->interval_id) {
        return;
    }

    if (ns->traffic_metrics_interval >= 0 &&
        m->interval_id == ns->traffic_metrics_interval + 1) {
        ns->ingress_mbit_previous = ns->ingress_mbit_current;
        ns->egress_mbit_previous = ns->egress_mbit_current;
        ns->dropped_mbit_previous = ns->dropped_mbit_current;
    } else {
        ns->ingress_mbit_previous = 0.0;
        ns->egress_mbit_previous = 0.0;
        ns->dropped_mbit_previous = 0.0;
    }

    ns->traffic_metrics_interval = m->interval_id;
    ns->ingress_mbit_current = 0.0;
    ns->egress_mbit_current = 0.0;
    ns->dropped_mbit_current = 0.0;
}

static void restore_switch_traffic_metrics_event(switch_state* ns, const fluid_msg* m) {
    ns->traffic_metrics_interval = m->rc_prev_traffic_metrics_interval;
    ns->ingress_mbit_current = m->rc_prev_ingress_mbit_current;
    ns->ingress_mbit_previous = m->rc_prev_ingress_mbit_previous;
    ns->egress_mbit_current = m->rc_prev_egress_mbit_current;
    ns->egress_mbit_previous = m->rc_prev_egress_mbit_previous;
    ns->dropped_mbit_current = m->rc_prev_dropped_mbit_current;
    ns->dropped_mbit_previous = m->rc_prev_dropped_mbit_previous;
}

static std::vector<int> select_ml_candidate_indices(const std::vector<queued_flowlet>& qv) {
    std::vector<int> indices;
    indices.reserve(qv.size());
    for (int i = 0; i < (int)qv.size(); ++i) {
        if (qv[i].valid && qv[i].remaining_mbit > EPS) {
            indices.push_back(i);
        }
    }

    auto tie_break_less = [&](int lhs, int rhs) {
        if (qv[lhs].enqueue_interval != qv[rhs].enqueue_interval) {
            return qv[lhs].enqueue_interval < qv[rhs].enqueue_interval;
        }
        if (qv[lhs].flowlet_id != qv[rhs].flowlet_id) {
            return qv[lhs].flowlet_id < qv[rhs].flowlet_id;
        }
        return lhs < rhs;
    };

    if (strcmp(cfg.director_candidate_selection_policy, "fifo") == 0) {
        std::stable_sort(indices.begin(), indices.end(), tie_break_less);
    } else if (strcmp(cfg.director_candidate_selection_policy, "sjf") == 0) {
        std::stable_sort(indices.begin(), indices.end(), [&](int lhs, int rhs) {
            if (std::fabs(qv[lhs].remaining_mbit - qv[rhs].remaining_mbit) > EPS) {
                return qv[lhs].remaining_mbit < qv[rhs].remaining_mbit;
            }
            return tie_break_less(lhs, rhs);
        });
    } else if (strcmp(cfg.director_candidate_selection_policy, "ljf") == 0) {
        std::stable_sort(indices.begin(), indices.end(), [&](int lhs, int rhs) {
            if (std::fabs(qv[lhs].remaining_mbit - qv[rhs].remaining_mbit) > EPS) {
                return qv[lhs].remaining_mbit > qv[rhs].remaining_mbit;
            }
            return tie_break_less(lhs, rhs);
        });
    }

    if ((int)indices.size() > cfg.director_max_candidate_flowlets) {
        indices.resize(cfg.director_max_candidate_flowlets);
    }
    return indices;
}

static void snapshot_ml_candidates(const switch_state* ns, int port_id,
                                   const std::vector<int>& candidate_indices, fluid_msg* m) {
    m->rc_ml_candidate_count = 0;
    m->rc_ml_total_queued_flowlets = active_flowlet_count_on_port(ns, port_id);
    m->rc_ml_total_active_flowlets = active_flowlet_count_on_switch(ns);
    m->rc_ml_used_fallback = 0;
    m->rc_ml_ingress_mbit_current = ns->ingress_mbit_current;
    m->rc_ml_ingress_mbit_previous = ns->ingress_mbit_previous;
    m->rc_ml_egress_mbit_current = ns->egress_mbit_current;
    m->rc_ml_egress_mbit_previous = ns->egress_mbit_previous;
    m->rc_ml_dropped_mbit_current = ns->dropped_mbit_current;
    m->rc_ml_dropped_mbit_previous = ns->dropped_mbit_previous;

    for (int rank = 0; rank < (int)candidate_indices.size(); ++rank) {
        int queue_index = candidate_indices[rank];
        if (queue_index < 0 || queue_index >= (int)ns->queues[port_id]->size()) {
            tw_error(TW_LOC, "invalid ML candidate queue index %d", queue_index);
        }
        ml_candidate_record* candidate = &m->rc_ml_candidates[m->rc_ml_candidate_count++];
        memset(candidate, 0, sizeof(*candidate));
        candidate->valid = 1;
        candidate->queue_index = queue_index;
        candidate->candidate_rank = rank;
        candidate->before = (*ns->queues[port_id])[queue_index];
    }
}

static int find_queue_index_for_flowlet(const switch_state* ns, int port_id,
                                        const queued_flowlet& needle) {
    if (port_id < 0 || port_id >= ns->num_ports || ns->queues[port_id] == NULL) {
        return -1;
    }

    const std::vector<queued_flowlet>& qv = *ns->queues[port_id];

    for (int i = 0; i < (int)qv.size(); ++i) {
        if (!qv[i].valid) {
            continue;
        }

        if (qv[i].flowlet_id == needle.flowlet_id &&
            qv[i].source_terminal == needle.source_terminal &&
            qv[i].destination_terminal == needle.destination_terminal &&
            qv[i].creation_interval == needle.creation_interval) {
            return i;
        }
    }

    return -1;
}

static int find_queue_index_for_msg(const switch_state* ns, int port_id, const fluid_msg* m) {
    if (port_id < 0 || port_id >= ns->num_ports || ns->queues[port_id] == NULL) {
        return -1;
    }

    const std::vector<queued_flowlet>& qv = *ns->queues[port_id];

    for (int i = 0; i < (int)qv.size(); ++i) {
        if (!qv[i].valid) {
            continue;
        }

        if (qv[i].flowlet_id == m->flowlet_id && qv[i].source_terminal == m->source_terminal &&
            qv[i].destination_terminal == m->destination_terminal &&
            qv[i].creation_interval == m->creation_interval) {
            return i;
        }
    }

    return -1;
}

static void compact_port_queue(switch_state* ns, int port_id) {
    if (port_id < 0 || port_id >= ns->num_ports || ns->queues[port_id] == NULL) {
        return;
    }
    std::vector<queued_flowlet>& qv = *ns->queues[port_id];
    qv.erase(std::remove_if(qv.begin(), qv.end(),
                            [](const queued_flowlet& q) {
                                return !q.valid || q.remaining_mbit <= EPS;
                            }),
             qv.end());
}

static bool fluid_commit_logging_active = false;
static std::ostringstream terminal_log_buffer;
static std::ostringstream switch_log_buffer;
static std::ostringstream flowlet_log_buffer;
static std::ostringstream switch_training_log_buffer;

static bool fluid_optimistic_mode(void) {
    return g_tw_synchronization_protocol == OPTIMISTIC ||
           g_tw_synchronization_protocol == OPTIMISTIC_DEBUG ||
           g_tw_synchronization_protocol == OPTIMISTIC_REALTIME;
}

static bool fluid_csv_forward_logs_enabled(void) {
    return !fluid_optimistic_mode();
}

static bool fluid_csv_commit_logs_enabled(void) {
    return fluid_optimistic_mode();
}

static bool fluid_csv_logs_enabled(void) {
    return fluid_csv_forward_logs_enabled() || fluid_commit_logging_active;
}

static void fluid_commit_logging_begin(void) {
    fluid_commit_logging_active = true;
}

static void fluid_commit_logging_end(void) {
    fluid_commit_logging_active = false;
}

static void append_log_row(const char* path, std::ostringstream* log_buffer,
                           const std::string& row) {
    if (path[0] == '\0' || !fluid_csv_logs_enabled()) {
        return;
    }

    (*log_buffer) << row;
}

static void append_terminal_log(int interval_id, const char* event_name, int terminal_id,
                                int peer_id, double mbit) {
    std::ostringstream row;
    row << interval_id << ',' << event_name << ',' << terminal_id << ','
        << terminals[terminal_id].name << ',' << terminals[terminal_id].switch_id << ',' << peer_id
        << ',' << mbit << '\n';
    append_log_row(cfg.terminal_log_path, &terminal_log_buffer, row.str());
}

static void append_switch_log(int interval_id, const char* event_name, int switch_id, int port_id,
                              int target_is_terminal, int target_index, double capacity_mbit,
                              double queued_before_mbit, double sent_mbit, double queued_after_mbit,
                              double shared_queued_before_mbit, double shared_queued_after_mbit,
                              double shared_buffer_mbit, double dropped_mbit,
                              int active_queue_entries) {
    std::ostringstream row;
    row << interval_id << ',' << event_name << ',' << switch_id << ',' << switches[switch_id].name
        << ',' << port_id << ',' << (target_is_terminal ? "terminal" : "switch") << ','
        << target_index << ',' << capacity_mbit << ',' << queued_before_mbit << ',' << sent_mbit
        << ',' << queued_after_mbit << ',' << shared_queued_before_mbit << ','
        << shared_queued_after_mbit << ',' << shared_buffer_mbit << ',' << dropped_mbit << ','
        << active_queue_entries << '\n';
    append_log_row(cfg.switch_log_path, &switch_log_buffer, row.str());
}

static void append_flowlet_log(int interval_id, const char* event_name, int switch_id, int port_id,
                               int target_is_terminal, int target_index,
                               unsigned long long flowlet_id, int source_terminal,
                               int destination_terminal, int creation_interval,
                               double capacity_mbit, double queued_before_mbit, double send_mbit,
                               double remaining_after_mbit, double dropped_mbit) {
    std::ostringstream row;
    row << interval_id << ',' << event_name << ',' << switch_id << ',' << switches[switch_id].name
        << ',' << port_id << ',' << (target_is_terminal ? "terminal" : "switch") << ','
        << target_index << ',' << flowlet_id << ',' << source_terminal << ','
        << destination_terminal << ',' << creation_interval << ','
        << (interval_id - creation_interval) << ',' << capacity_mbit << ',' << queued_before_mbit
        << ',' << send_mbit << ',' << remaining_after_mbit << ',' << dropped_mbit << '\n';
    append_log_row(cfg.flowlet_log_path, &flowlet_log_buffer, row.str());
}

static void append_switch_training_log(const switch_state* ns, const fluid_msg* m) {
    if (m->rc_ml_candidate_count <= 0) {
        return;
    }

    double shared_occupancy = 0.0;
    if (ns->shared_buffer_mbit > EPS) {
        shared_occupancy = m->rc_log_shared_queued_before_mbit / ns->shared_buffer_mbit;
    }

    for (int i = 0; i < m->rc_ml_candidate_count; ++i) {
        const ml_candidate_record* candidate = &m->rc_ml_candidates[i];
        if (!candidate->valid) {
            continue;
        }

        const queued_flowlet& q = candidate->before;
        std::ostringstream row;
        row << ns->switch_id << '-' << m->interval_id << '-' << m->port_id << ','
            << m->interval_id << ',' << ns->switch_id << ',' << switches[ns->switch_id].name
            << ',' << m->port_id << ','
            << (m->rc_log_target_is_terminal ? "terminal" : "switch") << ','
            << m->rc_log_target_index << ',' << m->rc_log_target_is_terminal << ','
            << cfg.switch_scheduler << ',' << cfg.director_candidate_selection_policy << ','
            << m->rc_ml_used_fallback << ',' << m->rc_ml_total_queued_flowlets << ','
            << m->rc_ml_candidate_count << ',' << candidate->candidate_rank << ','
            << q.flowlet_id << ',' << q.source_terminal << ',' << q.destination_terminal << ','
            << q.creation_interval << ',' << q.enqueue_interval << ','
            << (m->interval_id - q.creation_interval) << ','
            << (m->interval_id - q.enqueue_interval) << ',' << q.remaining_mbit << ','
            << candidate->allocation_mbit << ',' << m->rc_log_capacity_mbit << ','
            << m->rc_log_port_queued_before_mbit << ','
            << m->rc_log_port_queued_after_mbit << ','
            << m->rc_log_shared_queued_before_mbit << ','
            << m->rc_log_shared_queued_after_mbit << ',' << ns->shared_buffer_mbit << ','
            << shared_occupancy << ',' << m->rc_log_sent_total_mbit << ','
            << m->rc_ml_ingress_mbit_current << ','
            << m->rc_ml_ingress_mbit_previous << ',' << m->rc_ml_egress_mbit_current << ','
            << m->rc_ml_egress_mbit_previous << ',' << m->rc_ml_dropped_mbit_current << ','
            << m->rc_ml_dropped_mbit_previous << ',' << m->rc_ml_total_active_flowlets << ','
            << m->rc_log_active_before_entries << ',' << m->rc_log_active_after_entries << '\n';
        append_log_row(cfg.switch_training_log_path, &switch_training_log_buffer, row.str());
    }
}

static int next_terminal_generate_interval_after(int interval_id) {
    int period = cfg.terminal_send_every_n_intervals;
    if (period <= 0) {
        period = 1;
    }

    if (interval_id < 0) {
        return 0;
    }

    return interval_id + period;
}

static int first_terminal_generate_interval(void) {
    /*
     * The first eligible terminal workload interval is always 0.  Later events
     * advance by terminal_send_every_n_intervals, so terminals do not carry a
     * speculative self-event chain through intervals where no workload can be
     * generated.
     */
    return 0;
}

static void schedule_workload_generate(terminal_state* ns, int interval_id, tw_lp* lp) {
    if (interval_id >= cfg.num_send_intervals) {
        return;
    }

    int period = cfg.terminal_send_every_n_intervals;
    if (period <= 0) {
        period = 1;
    }

    /*
     * Defensive alignment: callers should already pass eligible send intervals,
     * but if a future caller passes an arbitrary interval, round up to the next
     * eligible workload-generation interval instead of scheduling a no-op event.
     */
    int rem = interval_id % period;
    if (rem != 0) {
        interval_id += period - rem;
    }

    if (interval_id >= cfg.num_send_intervals) {
        return;
    }

    tw_event* e = tw_event_new(lp->gid, delay_until_ns(interval_id, PHASE_GENERATE, lp), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = WORKLOAD_GENERATE;
    m->interval_id = interval_id;
    m->source_terminal = ns->terminal_id;
    tw_event_send(e);
}

static void schedule_switch_egress(int interval_id, int port_id, tw_lp* lp) {
    if (interval_id >= cfg.num_send_intervals + cfg.num_drain_intervals) {
        return;
    }
    tw_event* e = tw_event_new(lp->gid, delay_until_ns(interval_id, PHASE_SWITCH_EGRESS, lp), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = SWITCH_EGRESS;
    m->interval_id = interval_id;
    m->port_id = port_id;
    tw_event_send(e);
}

static void request_switch_egress(switch_state* ns, int interval_id, int port_id, tw_lp* lp,
                                  fluid_msg* cause_msg) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid request_switch_egress port %d on switch %d", port_id,
                 ns->switch_id);
    }

    int prev = ns->scheduled_egress_interval[port_id];

    if (cause_msg != NULL) {
        cause_msg->rc_prev_scheduled_egress_interval = prev;
    }

    if (interval_id >= cfg.num_send_intervals + cfg.num_drain_intervals) {
        return;
    }

    if (prev == interval_id) {
        return;
    }

    if (prev >= 0 && prev < interval_id) {
        /*
         * An earlier egress is already outstanding.  Let that earlier event
         * decide whether residual bytes require a later egress.
         */
        return;
    }

    if (prev >= 0 && prev > interval_id) {
        tw_error(TW_LOC,
                 "switch %d port %d has future egress interval %d while trying "
                 "to schedule earlier interval %d",
                 ns->switch_id, port_id, prev, interval_id);
    }

    schedule_switch_egress(interval_id, port_id, lp);
    ns->scheduled_egress_interval[port_id] = interval_id;

    if (cause_msg != NULL) {
    }
}

static void schedule_arrival(int interval_id, tw_lpid dst_gid, const fluid_msg* src_msg,
                             double mbit, int dst_switch, tw_lp* lp) {
    tw_event* e = tw_event_new(dst_gid, delay_until_ns(interval_id, PHASE_ARRIVAL, lp), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = FLOWLET_ARRIVAL;
    m->interval_id = interval_id;
    m->source_terminal = src_msg->source_terminal;
    m->destination_terminal = src_msg->destination_terminal;
    m->source_switch = src_msg->source_switch;
    m->destination_switch = dst_switch;
    m->creation_interval = src_msg->creation_interval;
    m->flowlet_id = src_msg->flowlet_id;
    m->mbit = mbit;
    tw_event_send(e);
}

static void terminal_init(terminal_state* ns, tw_lp* lp) {
    memset(ns, 0, sizeof(*ns));
    ns->terminal_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    if (ns->terminal_id < 0 || ns->terminal_id >= (int)terminals.size()) {
        tw_error(TW_LOC, "terminal LP relative id %d out of range", ns->terminal_id);
    }
    ns->attached_switch = terminals[ns->terminal_id].switch_id;
    ns->local_terminal_id = terminals[ns->terminal_id].local_id;
    ns->next_flowlet_seq = 0;
    schedule_workload_generate(ns, first_terminal_generate_interval(), lp);
}

static int choose_random_destination(int self, tw_lp* lp) {
    int n = (int)terminals.size();
    if (n <= 1) {
        return self;
    }
    int offset = (int)tw_rand_integer(lp->rng, 1, n - 1);
    return (self + offset) % n;
}

static double random_workload_mbit(int terminal_id, tw_lp* lp) {
    int sw = terminals[terminal_id].switch_id;
    double cap = switches[sw].terminal_bandwidth_mbps * cfg.interval_seconds;
    double max_mbit = cap * cfg.terminal_max_send_fraction_of_link_capacity;
    double min_mbit = std::min(cfg.terminal_min_send_mbit, max_mbit);
    double u = tw_rand_unif(lp->rng);
    return min_mbit + u * (max_mbit - min_mbit);
}


static void log_terminal_generate_event(const terminal_state* ns, const fluid_msg* m) {
    if (m->rc_generated) {
        append_terminal_log(m->interval_id, "generate_send", ns->terminal_id,
                            m->destination_terminal, m->mbit);
    }
}

static void log_terminal_receive_event(const terminal_state* ns, const fluid_msg* m) {
    append_terminal_log(m->interval_id, "receive", ns->terminal_id, m->source_terminal, m->mbit);
}

static void log_switch_arrival_event(const switch_state* ns, const fluid_msg* m) {
    if (m->rc_no_route) {
        append_switch_log(m->interval_id, "drop_no_route", ns->switch_id, -1, 0, -1, 0.0, 0.0, 0.0,
                          0.0, m->rc_log_shared_queued_before_mbit,
                          m->rc_log_shared_queued_after_mbit, ns->shared_buffer_mbit,
                          m->rc_dropped_mbit, 0);
        append_flowlet_log(m->interval_id, "drop_no_route", ns->switch_id, -1, 0, -1, m->flowlet_id,
                           m->source_terminal, m->destination_terminal, m->creation_interval, 0.0,
                           0.0, 0.0, 0.0, m->rc_dropped_mbit);
        return;
    }

    if (m->rc_accepted_mbit > EPS) {
        append_flowlet_log(m->interval_id, m->rc_coalesced ? "enqueue_coalesce" : "enqueue",
                           ns->switch_id, m->rc_port_id, m->rc_log_target_is_terminal,
                           m->rc_log_target_index, m->flowlet_id, m->source_terminal,
                           m->destination_terminal, m->creation_interval, m->rc_log_capacity_mbit,
                           m->rc_log_port_queued_before_mbit, 0.0,
                           m->rc_log_flowlet_remaining_after_mbit, 0.0);
    }

    if (m->rc_dropped_mbit > EPS) {
        append_switch_log(m->interval_id, "drop_shared_buffer_overflow", ns->switch_id,
                          m->rc_port_id, m->rc_log_target_is_terminal, m->rc_log_target_index,
                          m->rc_log_capacity_mbit, m->rc_log_port_queued_before_mbit, 0.0,
                          m->rc_log_port_queued_after_mbit, m->rc_log_shared_queued_before_mbit,
                          m->rc_log_shared_queued_after_mbit, ns->shared_buffer_mbit,
                          m->rc_dropped_mbit, m->rc_log_active_after_entries);

        append_flowlet_log(m->interval_id, "drop_shared_buffer_overflow", ns->switch_id,
                           m->rc_port_id, m->rc_log_target_is_terminal, m->rc_log_target_index,
                           m->flowlet_id, m->source_terminal, m->destination_terminal,
                           m->creation_interval, m->rc_log_capacity_mbit,
                           m->rc_log_port_queued_before_mbit, 0.0,
                           m->rc_log_flowlet_remaining_after_mbit, m->rc_dropped_mbit);
    }
}

static void log_switch_egress_event(const switch_state* ns, const fluid_msg* m) {
    for (int i = 0; i < m->rc_alloc_count; ++i) {
        const rc_alloc_record* rc = &m->rc_allocs[i];

        if (!rc->valid || rc->send_mbit <= EPS) {
            continue;
        }

        double remaining_after = rc->before.remaining_mbit - rc->send_mbit;
        if (remaining_after < 0.0 && remaining_after > -EPS) {
            remaining_after = 0.0;
        }

        append_flowlet_log(m->interval_id, "allocate_send", ns->switch_id, m->port_id,
                           m->rc_log_target_is_terminal, m->rc_log_target_index,
                           rc->before.flowlet_id, rc->before.source_terminal,
                           rc->before.destination_terminal, rc->before.creation_interval,
                           m->rc_log_capacity_mbit, m->rc_log_port_queued_before_mbit,
                           rc->send_mbit, remaining_after, 0.0);
    }

    append_switch_log(m->interval_id, "egress", ns->switch_id, m->port_id,
                      m->rc_log_target_is_terminal, m->rc_log_target_index, m->rc_log_capacity_mbit,
                      m->rc_log_port_queued_before_mbit, m->rc_log_sent_total_mbit,
                      m->rc_log_port_queued_after_mbit, m->rc_log_shared_queued_before_mbit,
                      m->rc_log_shared_queued_after_mbit, ns->shared_buffer_mbit, 0.0,
                      m->rc_log_active_after_entries);

    append_switch_training_log(ns, m);
}

static void handle_workload_generate(terminal_state* ns, fluid_msg* m, tw_lp* lp) {
    int interval = m->interval_id;

    m->rc_rng_count = 0;
    m->rc_generated = 0;
    m->mbit = 0.0;
    m->destination_terminal = -1;
    m->flowlet_id = 0;

    double p = tw_rand_unif(lp->rng);
    m->rc_rng_count++;

    if (p <= cfg.terminal_send_probability) {
        int dst = choose_random_destination(ns->terminal_id, lp);
        m->rc_rng_count++;

        double mbit = random_workload_mbit(ns->terminal_id, lp);
        m->rc_rng_count++;

        if (mbit > EPS) {
            fluid_msg out_msg;
            memset(&out_msg, 0, sizeof(out_msg));
            out_msg.event_type = FLOWLET_ARRIVAL;
            out_msg.interval_id = interval + 1;
            out_msg.source_terminal = ns->terminal_id;
            out_msg.destination_terminal = dst;
            out_msg.source_switch = ns->attached_switch;
            out_msg.destination_switch = ns->attached_switch;
            out_msg.creation_interval = interval;
            out_msg.flowlet_id = ((unsigned long long)ns->terminal_id << 48) |
                                 (unsigned long long)ns->next_flowlet_seq++;
            out_msg.mbit = mbit;

            tw_lpid sw_gid = get_switch_gid(ns->attached_switch);
            schedule_arrival(interval + 1, sw_gid, &out_msg, mbit, ns->attached_switch, lp);

            ns->generated_mbit += mbit;
            ns->sent_to_switch_mbit += mbit;
            ns->generated_flowlets++;

            m->rc_generated = 1;
            m->destination_terminal = dst;
            m->flowlet_id = out_msg.flowlet_id;
            m->mbit = mbit;

            log_terminal_generate_event(ns, m);

            if (cfg.debug_prints) {
                printf("[fluid terminal] interval=%d terminal=%d dst=%d mbit=%.6f\n", interval,
                       ns->terminal_id, dst, mbit);
            }
        }
    }

    schedule_workload_generate(ns, next_terminal_generate_interval_after(interval), lp);
}


static void handle_terminal_arrival(terminal_state* ns, fluid_msg* m) {
    ns->received_mbit += m->mbit;
    ns->received_fragments++;
    log_terminal_receive_event(ns, m);
}

static void terminal_event(terminal_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;
    switch (m->event_type) {
    case WORKLOAD_GENERATE:
        handle_workload_generate(ns, m, lp);
        break;
    case FLOWLET_ARRIVAL:
        handle_terminal_arrival(ns, m);
        break;
    default:
        tw_error(TW_LOC, "terminal received unknown event type %d", m->event_type);
    }
}

static void terminal_rev_event(terminal_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;

    switch (m->event_type) {
    case WORKLOAD_GENERATE:
        if (m->rc_generated) {
            ns->generated_mbit -= m->mbit;
            ns->sent_to_switch_mbit -= m->mbit;
            ns->generated_flowlets--;

            if (ns->next_flowlet_seq == 0) {
                tw_error(TW_LOC, "terminal %d next_flowlet_seq underflow during rollback",
                         ns->terminal_id);
            }

            ns->next_flowlet_seq--;
        }

        for (int i = 0; i < m->rc_rng_count; ++i) {
            tw_rand_reverse_unif(lp->rng);
        }

        break;

    case FLOWLET_ARRIVAL:
        ns->received_mbit -= m->mbit;
        ns->received_fragments--;
        break;

    default:
        tw_error(TW_LOC, "terminal reverse received unknown event type %d", m->event_type);
    }
}

static void terminal_commit_event(terminal_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;
    (void)lp;

    if (!fluid_csv_commit_logs_enabled()) {
        return;
    }

    fluid_commit_logging_begin();

    switch (m->event_type) {
    case WORKLOAD_GENERATE:
        log_terminal_generate_event(ns, m);
        break;

    case FLOWLET_ARRIVAL:
        log_terminal_receive_event(ns, m);
        break;

    default:
        break;
    }

    fluid_commit_logging_end();
}

static void terminal_finalize(terminal_state* ns, tw_lp* lp) {
    if (cfg.debug_prints) {
        printf(
            "fluid-flow-wan-terminal gid=%llu terminal=%d switch=%d "
            "generated_mbit=%.6f sent_mbit=%.6f received_mbit=%.6f "
            "generated_flowlets=%d received_fragments=%d\n",
            (unsigned long long)lp->gid, ns->terminal_id,
            ns->attached_switch, ns->generated_mbit,
            ns->sent_to_switch_mbit, ns->received_mbit,
            ns->generated_flowlets, ns->received_fragments);
    }
}

static void switch_init(switch_state* ns, tw_lp* lp) {
    memset(ns, 0, sizeof(*ns));
    ns->switch_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    ns->traffic_metrics_interval = -1;
    if (ns->switch_id < 0 || ns->switch_id >= (int)switches.size()) {
        tw_error(TW_LOC, "switch LP relative id %d out of range", ns->switch_id);
    }

    for (int p = 0; p < MAX_PORTS_PER_SWITCH; ++p) {
        ns->scheduled_egress_interval[p] = -1;
        ns->rr_next_queue_index[p] = 0;
    }

    const switch_info& sw = switches[ns->switch_id];

    /*
     * switch_buffer is modeled as one shared switch-wide pool. Output queues
     * remain per port for scheduling, but admission checks total queued bytes
     * across all ports on this switch.
     */
    ns->shared_buffer_mbit = sw.switch_buffer_mbit;
    if (ns->shared_buffer_mbit < 0.0) {
        tw_error(TW_LOC, "negative shared switch buffer on switch %d", ns->switch_id);
    }

    for (size_t i = 0; i < sw.links.size(); ++i) {
        if (ns->num_ports >= MAX_PORTS_PER_SWITCH) {
            tw_error(TW_LOC, "too many ports on switch %d", ns->switch_id);
        }
        port_desc* p = &ns->ports[ns->num_ports++];
        memset(p, 0, sizeof(*p));
        p->is_terminal = 0;
        p->target_index = sw.links[i].dst_switch;
        p->capacity_mbit_per_interval = sw.links[i].bandwidth_mbps * cfg.interval_seconds;
        ns->queues[ns->num_ports - 1] = new std::vector<queued_flowlet>();
    }
    for (int t = 0; t < sw.terminal_count; ++t) {
        if (ns->num_ports >= MAX_PORTS_PER_SWITCH) {
            tw_error(TW_LOC, "too many ports on switch %d", ns->switch_id);
        }
        port_desc* p = &ns->ports[ns->num_ports++];
        memset(p, 0, sizeof(*p));
        p->is_terminal = 1;
        p->target_index = sw.terminal_start + t;
        p->capacity_mbit_per_interval = sw.terminal_bandwidth_mbps * cfg.interval_seconds;
        ns->queues[ns->num_ports - 1] = new std::vector<queued_flowlet>();
    }

    /*
     * Egress is demand-driven.  We no longer pre-schedule periodic empty
     * egress events for every port.  Arrivals schedule the first egress, and
     * egress events reschedule themselves only while residual queued bytes
     * remain.
     */
}


static void handle_switch_arrival(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    ns->received_fragments++;
    ns->ingress_mbit_current += m->mbit;
    int dst_sw = terminals[m->destination_terminal].switch_id;
    int port_id = -1;
    if (dst_sw == ns->switch_id) {
        port_id = find_terminal_port(ns, m->destination_terminal);
    } else {
        int next_sw = next_switch_table[ns->switch_id][dst_sw];
        if (next_sw >= 0) {
            port_id = find_switch_link_port(ns, next_sw);
        }
    }

    if (port_id < 0) {
        double shared_before = queued_mbit_on_switch(ns);
        m->rc_no_route = 1;
        m->rc_port_id = -1;
        m->rc_accepted_mbit = 0.0;
        m->rc_dropped_mbit = m->mbit;
        m->rc_prev_scheduled_egress_interval = -1;
        m->rc_log_target_is_terminal = 0;
        m->rc_log_target_index = -1;
        m->rc_log_capacity_mbit = 0.0;
        m->rc_log_port_queued_before_mbit = 0.0;
        m->rc_log_port_queued_after_mbit = 0.0;
        m->rc_log_shared_queued_before_mbit = shared_before;
        m->rc_log_shared_queued_after_mbit = shared_before;
        m->rc_log_flowlet_remaining_after_mbit = 0.0;
        m->rc_log_sent_total_mbit = 0.0;
        m->rc_log_active_before_entries = 0;
        m->rc_log_active_after_entries = 0;
        ns->dropped_mbit += m->mbit;
        ns->dropped_mbit_current += m->mbit;
        log_switch_arrival_event(ns, m);
        return;
    }

    port_desc* p = &ns->ports[port_id];
    double port_queued_before = 0.0;
    double shared_queued_before = 0.0;
    double shared_queued_after = 0.0;
    double dropped = 0.0;
    double flowlet_remaining_after = 0.0;
    int coalesced = 0;
    int queue_index = -1;

    m->rc_no_route = 0;
    m->rc_port_id = port_id;
    m->rc_queue_index = -1;
    m->rc_coalesced = 0;
    m->rc_accepted_mbit = 0.0;
    m->rc_dropped_mbit = 0.0;
    m->rc_prev_scheduled_egress_interval = ns->scheduled_egress_interval[port_id];
    m->rc_log_target_is_terminal = p->is_terminal;
    m->rc_log_target_index = p->target_index;
    m->rc_log_capacity_mbit = p->capacity_mbit_per_interval;
    m->rc_log_port_queued_before_mbit = 0.0;
    m->rc_log_port_queued_after_mbit = 0.0;
    m->rc_log_shared_queued_before_mbit = 0.0;
    m->rc_log_shared_queued_after_mbit = 0.0;
    m->rc_log_flowlet_remaining_after_mbit = 0.0;
    m->rc_log_sent_total_mbit = 0.0;
    m->rc_log_active_before_entries = 0;
    m->rc_log_active_after_entries = 0;

    double accepted = enqueue_flowlet(ns, port_id, m, &port_queued_before, &shared_queued_before,
                                      &shared_queued_after, &dropped, &flowlet_remaining_after,
                                      &coalesced, &queue_index);

    m->rc_queue_index = queue_index;
    m->rc_coalesced = coalesced;
    m->rc_accepted_mbit = accepted;
    m->rc_dropped_mbit = dropped;
    ns->dropped_mbit_current += dropped;

    double port_queued_after = queued_mbit_on_port(ns, port_id);
    int active_after = active_flowlet_count_on_port(ns, port_id);

    m->rc_log_port_queued_before_mbit = port_queued_before;
    m->rc_log_port_queued_after_mbit = port_queued_after;
    m->rc_log_shared_queued_before_mbit = shared_queued_before;
    m->rc_log_shared_queued_after_mbit = shared_queued_after;
    m->rc_log_flowlet_remaining_after_mbit = flowlet_remaining_after;
    m->rc_log_active_after_entries = active_after;

    log_switch_arrival_event(ns, m);

    if (port_queued_after > EPS) {
        request_switch_egress(ns, m->interval_id, port_id, lp, m);
    }
}


static void send_flowlet_fragment(switch_state* ns, int port_id, const queued_flowlet* q,
                                  double send_mbit, int interval_id, tw_lp* lp) {
    if (send_mbit <= EPS) {
        return;
    }
    fluid_msg out_msg;
    memset(&out_msg, 0, sizeof(out_msg));
    out_msg.event_type = FLOWLET_ARRIVAL;
    out_msg.interval_id = interval_id + 1;
    out_msg.source_terminal = q->source_terminal;
    out_msg.destination_terminal = q->destination_terminal;
    out_msg.source_switch = ns->switch_id;
    out_msg.creation_interval = q->creation_interval;
    out_msg.flowlet_id = q->flowlet_id;
    out_msg.mbit = send_mbit;

    const port_desc* p = &ns->ports[port_id];
    if (p->is_terminal) {
        out_msg.destination_switch = ns->switch_id;
        tw_lpid term_gid = get_terminal_gid(p->target_index);
        schedule_arrival(interval_id + 1, term_gid, &out_msg, send_mbit, ns->switch_id, lp);
        ns->delivered_local_mbit += send_mbit;
    } else {
        out_msg.destination_switch = p->target_index;
        tw_lpid sw_gid = get_switch_gid(p->target_index);
        schedule_arrival(interval_id + 1, sw_gid, &out_msg, send_mbit, p->target_index, lp);
    }
    ns->sent_mbit += send_mbit;
    ns->sent_fragments++;
}

static std::unordered_map<std::string, std::vector<double>> fluid_flow_wan_inference_cache;

static std::string build_fluid_flow_wan_inference_payload(const switch_state* ns,
                                                        const fluid_msg* m) {
    std::ostringstream out;
    out << std::setprecision(17);

    double occupancy = 0.0;
    if (ns->shared_buffer_mbit > EPS) {
        occupancy = m->rc_log_shared_queued_before_mbit / ns->shared_buffer_mbit;
    }

    out << "{\"schema\":\"fluid-flow-wan-v1\",\"global_features\":["
        << ns->shared_buffer_mbit << ','
        << m->rc_log_shared_queued_before_mbit << ',' << occupancy << ','
        << m->rc_ml_ingress_mbit_current << ',' << m->rc_ml_ingress_mbit_previous << ','
        << m->rc_ml_egress_mbit_current << ',' << m->rc_ml_egress_mbit_previous << ','
        << m->rc_ml_dropped_mbit_current << ',' << m->rc_ml_dropped_mbit_previous << ','
        << m->rc_ml_total_active_flowlets << "],\"port_features\":["
        << m->port_id << ',' << m->rc_log_target_index << ',' << m->rc_log_capacity_mbit << ','
        << m->rc_log_port_queued_before_mbit << ',' << m->rc_log_active_before_entries << ','
        << m->rc_log_target_is_terminal
        << "],\"candidate_features\":[";

    for (int i = 0; i < MAX_ML_CANDIDATES; ++i) {
        if (i > 0) {
            out << ',';
        }
        if (i < m->rc_ml_candidate_count && m->rc_ml_candidates[i].valid) {
            const ml_candidate_record& candidate = m->rc_ml_candidates[i];
            const queued_flowlet& q = candidate.before;
            out << '[' << q.remaining_mbit << ',' << (m->interval_id - q.creation_interval) << ','
                << (m->interval_id - q.enqueue_interval) << ',' << q.source_terminal << ','
                << q.destination_terminal << ',' << candidate.candidate_rank << ']';
        } else {
            out << "[0,0,0,0,0,0]";
        }
    }

    out << "],\"candidate_mask\":[";
    for (int i = 0; i < MAX_ML_CANDIDATES; ++i) {
        if (i > 0) {
            out << ',';
        }
        out << (i < m->rc_ml_candidate_count && m->rc_ml_candidates[i].valid ? 1 : 0);
    }
    out << "]}";
    return out.str();
}

static bool request_fluid_flow_wan_weights(const switch_state* ns, const fluid_msg* m,
                                         std::vector<double>* weights_out) {
    weights_out->clear();
    if (m->rc_ml_candidate_count <= 0) {
        return false;
    }

#if CODES_HAVE_ZEROMQ
    std::string payload = build_fluid_flow_wan_inference_payload(ns, m);
    std::string cache_key = std::to_string(ns->switch_id) + ":" + payload;
    auto cached = fluid_flow_wan_inference_cache.find(cache_key);
    if (cached != fluid_flow_wan_inference_cache.end()) {
        *weights_out = cached->second;
        return true;
    }

    try {
        std::vector<std::string> reply = zmqml_director_request(
            "fluid-flow-wan", "torchscript", "inference",
            {"1", std::to_string(ns->switch_id)}, payload,
            cfg.director_inference_timeout_ms, cfg.director_endpoint);
        if (reply.size() < 3 || reply[0] != "done") {
            return false;
        }

        std::istringstream predictions(reply[2]);
        double value = 0.0;
        while (predictions >> value) {
            weights_out->push_back(value);
        }

        if ((int)weights_out->size() != MAX_ML_CANDIDATES) {
            weights_out->clear();
            return false;
        }

        double positive_sum = 0.0;
        for (int i = 0; i < m->rc_ml_candidate_count; ++i) {
            double weight = (*weights_out)[i];
            if (!std::isfinite(weight) || weight < 0.0) {
                weights_out->clear();
                return false;
            }
            positive_sum += weight;
        }
        if (positive_sum <= EPS) {
            weights_out->clear();
            return false;
        }

        fluid_flow_wan_inference_cache.emplace(cache_key, *weights_out);
        return true;
    } catch (const std::exception& ex) {
        if (cfg.debug_prints) {
            fprintf(stderr,
                    "[fluid-flow-wan ML] inference failed on switch %d port %d interval %d: %s\n",
                    ns->switch_id, m->port_id, m->interval_id, ex.what());
        }
        weights_out->clear();
        return false;
    }
#else
    (void)ns;
    (void)m;
    return false;
#endif
}

static void handle_switch_egress(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    int port_id = m->port_id;
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid switch egress port %d", port_id);
    }
    if (ns->queues[port_id] == NULL) {
        m->rc_prev_scheduled_egress_interval = -1;
        m->rc_prev_rr_next_queue_index = 0;
        return;
    }

    port_desc* p = &ns->ports[port_id];
    std::vector<queued_flowlet>& qv = *ns->queues[port_id];

    m->rc_prev_scheduled_egress_interval = ns->scheduled_egress_interval[port_id];
    m->rc_prev_rr_next_queue_index = ns->rr_next_queue_index[port_id];
    if (ns->scheduled_egress_interval[port_id] == m->interval_id) {
        ns->scheduled_egress_interval[port_id] = -1;
    }

    double capacity = p->capacity_mbit_per_interval;
    double remaining_capacity = capacity;
    double sent_total = 0.0;
    double queued_before = queued_mbit_on_port(ns, port_id);
    double shared_queued_before = queued_mbit_on_switch(ns);
    int active_before = active_flowlet_count_on_port(ns, port_id);

    std::vector<double> send_plan(qv.size(), 0.0);
    m->rc_alloc_count = 0;
    m->rc_log_target_is_terminal = p->is_terminal;
    m->rc_log_target_index = p->target_index;
    m->rc_log_capacity_mbit = capacity;
    m->rc_log_port_queued_before_mbit = queued_before;
    m->rc_log_port_queued_after_mbit = queued_before;
    m->rc_log_shared_queued_before_mbit = shared_queued_before;
    m->rc_log_shared_queued_after_mbit = shared_queued_before;
    m->rc_log_flowlet_remaining_after_mbit = 0.0;
    m->rc_log_sent_total_mbit = 0.0;
    m->rc_log_active_before_entries = active_before;
    m->rc_log_active_after_entries = active_before;

    std::vector<int> candidate_indices = select_ml_candidate_indices(qv);
    snapshot_ml_candidates(ns, port_id, candidate_indices, m);

    auto add_to_plan = [&](int idx, double requested_send) -> double {
        if (idx < 0 || idx >= (int)send_plan.size() || requested_send <= EPS) {
            return 0.0;
        }

        double still_available_for_flowlet = qv[idx].remaining_mbit - send_plan[idx];
        if (still_available_for_flowlet <= EPS) {
            return 0.0;
        }

        double actual_send = std::min(requested_send, still_available_for_flowlet);
        actual_send = std::min(actual_send, remaining_capacity);
        send_plan[idx] += actual_send;
        remaining_capacity -= actual_send;
        return actual_send;
    };

    bool used_round_robin_plan = false;

    auto build_fifo_plan = [&]() {
        for (int i = 0; i < (int)qv.size() && remaining_capacity > EPS; ++i) {
            if (!qv[i].valid || qv[i].remaining_mbit <= EPS) {
                continue;
            }
            add_to_plan(i, qv[i].remaining_mbit);
        }
    };

    auto build_round_robin_plan = [&]() {
        used_round_robin_plan = true;
        int qsize = (int)qv.size();
        int entries_to_serve = std::min(active_before, cfg.round_robin_max_entries_per_egress);
        if (qsize <= 0 || entries_to_serve <= 0 || remaining_capacity <= EPS) {
            return;
        }

        int idx = ns->rr_next_queue_index[port_id];
        if (idx < 0 || idx >= qsize) {
            idx = 0;
        }

        double quantum = cfg.round_robin_quantum_mbit;
        if (quantum <= EPS) {
            quantum = capacity;
        }

        int visited = 0;
        int served = 0;
        while (remaining_capacity > EPS && visited < qsize && served < entries_to_serve) {
            if (qv[idx].valid && qv[idx].remaining_mbit > EPS) {
                double before_capacity = remaining_capacity;
                add_to_plan(idx, std::min(qv[idx].remaining_mbit, quantum));
                if (remaining_capacity < before_capacity - EPS) {
                    served++;
                }
            }
            idx = (idx + 1) % qsize;
            visited++;
        }
        ns->rr_next_queue_index[port_id] = idx;
    };

    auto build_configured_fallback_plan = [&]() {
        m->rc_ml_used_fallback = 1;
        if (strcmp(cfg.director_fallback_scheduler, "fifo") == 0) {
            build_fifo_plan();
        } else {
            build_round_robin_plan();
        }
    };

    if (strcmp(cfg.switch_scheduler, "fifo") == 0) {
        build_fifo_plan();
    } else if (strcmp(cfg.switch_scheduler, "round_robin") == 0) {
        build_round_robin_plan();
    } else if (strcmp(cfg.switch_scheduler, "ml") == 0) {
        std::vector<double> weights;
        bool inference_ok = request_fluid_flow_wan_weights(ns, m, &weights);
        if (inference_ok) {
            std::vector<double> candidate_remaining(m->rc_ml_candidate_count, 0.0);
            for (int i = 0; i < m->rc_ml_candidate_count; ++i) {
                candidate_remaining[i] = m->rc_ml_candidates[i].before.remaining_mbit;
            }

            while (remaining_capacity > EPS) {
                double active_weight_sum = 0.0;
                for (int i = 0; i < m->rc_ml_candidate_count; ++i) {
                    if (candidate_remaining[i] > EPS && weights[i] > EPS) {
                        active_weight_sum += weights[i];
                    }
                }
                if (active_weight_sum <= EPS) {
                    break;
                }

                double round_capacity = remaining_capacity;
                double progress = 0.0;
                for (int i = 0; i < m->rc_ml_candidate_count && remaining_capacity > EPS; ++i) {
                    if (candidate_remaining[i] <= EPS || weights[i] <= EPS) {
                        continue;
                    }
                    double requested = round_capacity * weights[i] / active_weight_sum;
                    requested = std::min(requested, candidate_remaining[i]);
                    double sent = add_to_plan(m->rc_ml_candidates[i].queue_index, requested);
                    candidate_remaining[i] -= sent;
                    progress += sent;
                }
                if (progress <= EPS) {
                    break;
                }
            }

            /* Keep the output link work-conserving when selected candidates are too small. */
            if (remaining_capacity > EPS && queued_before > capacity - remaining_capacity + EPS) {
                build_configured_fallback_plan();
            }
        } else {
            build_configured_fallback_plan();
        }
    } else {
        tw_error(TW_LOC, "unknown switch scheduler '%s'", cfg.switch_scheduler);
    }

    for (int i = 0; i < m->rc_ml_candidate_count; ++i) {
        int queue_index = m->rc_ml_candidates[i].queue_index;
        if (queue_index >= 0 && queue_index < (int)send_plan.size()) {
            m->rc_ml_candidates[i].allocation_mbit = send_plan[queue_index];
        }
    }

    for (int i = 0; i < (int)qv.size(); ++i) {
        double send = send_plan[i];
        if (!qv[i].valid || send <= EPS) {
            continue;
        }
        if (send > qv[i].remaining_mbit + EPS) {
            tw_error(TW_LOC,
                     "computed send %.12f exceeds remaining %.12f for flowlet %llu "
                     "on switch %d port %d",
                     send, qv[i].remaining_mbit, (unsigned long long)qv[i].flowlet_id,
                     ns->switch_id, port_id);
        }

        queued_flowlet before = qv[i];
        send = std::min(send, qv[i].remaining_mbit);
        if (m->rc_alloc_count >= MAX_RC_ALLOCATIONS) {
            tw_error(TW_LOC,
                     "too many egress allocations for reverse metadata: switch %d port %d "
                     "interval %d count %d max %d",
                     ns->switch_id, port_id, m->interval_id, m->rc_alloc_count, MAX_RC_ALLOCATIONS);
        }

        rc_alloc_record* rc = &m->rc_allocs[m->rc_alloc_count++];
        memset(rc, 0, sizeof(*rc));
        rc->valid = 1;
        rc->queue_index = i;
        rc->before = before;
        rc->send_mbit = send;

        send_flowlet_fragment(ns, port_id, &before, send, m->interval_id, lp);
        qv[i].remaining_mbit -= send;
        sent_total += send;
    }

    ns->egress_mbit_current += sent_total;
    compact_port_queue(ns, port_id);

    if (used_round_robin_plan) {
        if (qv.empty()) {
            ns->rr_next_queue_index[port_id] = 0;
        } else {
            ns->rr_next_queue_index[port_id] %= (int)qv.size();
        }
    }

    double queued_after = queued_mbit_on_port(ns, port_id);
    double shared_queued_after = queued_mbit_on_switch(ns);
    int active_after = active_flowlet_count_on_port(ns, port_id);

    m->rc_log_port_queued_after_mbit = queued_after;
    m->rc_log_shared_queued_after_mbit = shared_queued_after;
    m->rc_log_sent_total_mbit = sent_total;
    m->rc_log_active_after_entries = active_after;

    log_switch_egress_event(ns, m);

    if (queued_after > EPS) {
        request_switch_egress(ns, m->interval_id + 1, port_id, lp, NULL);
    }
}


static void switch_event(switch_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;
    begin_switch_traffic_metrics_event(ns, m);
    switch (m->event_type) {
    case FLOWLET_ARRIVAL:
        handle_switch_arrival(ns, m, lp);
        break;
    case SWITCH_EGRESS:
        handle_switch_egress(ns, m, lp);
        break;
    default:
        tw_error(TW_LOC, "switch received unknown event type %d", m->event_type);
    }
}

static void rollback_switch_arrival(switch_state* ns, fluid_msg* m) {
    ns->received_fragments--;

    if (!m->rc_no_route && m->rc_port_id >= 0 && m->rc_port_id < ns->num_ports) {
        ns->scheduled_egress_interval[m->rc_port_id] = m->rc_prev_scheduled_egress_interval;
    }

    if (m->rc_no_route) {
        ns->dropped_mbit -= m->rc_dropped_mbit;
        return;
    }

    int port_id = m->rc_port_id;

    if (port_id < 0 || port_id >= ns->num_ports || ns->queues[port_id] == NULL) {
        tw_error(TW_LOC, "invalid rollback arrival port %d on switch %d", port_id, ns->switch_id);
    }

    ns->dropped_mbit -= m->rc_dropped_mbit;

    if (m->rc_accepted_mbit <= EPS) {
        return;
    }

    std::vector<queued_flowlet>& qv = *ns->queues[port_id];

    int idx = m->rc_queue_index;
    if (idx < 0 || idx >= (int)qv.size() || qv[idx].flowlet_id != m->flowlet_id) {
        idx = find_queue_index_for_msg(ns, port_id, m);
    }

    if (idx < 0) {
        tw_error(TW_LOC, "could not find flowlet %llu on switch %d port %d during arrival rollback",
                 (unsigned long long)m->flowlet_id, ns->switch_id, port_id);
    }

    if (m->rc_coalesced) {
        qv[idx].remaining_mbit -= m->rc_accepted_mbit;

        if (qv[idx].remaining_mbit <= EPS) {
            tw_error(
                TW_LOC,
                "coalesced flowlet %llu became empty during arrival rollback on switch %d port %d",
                (unsigned long long)m->flowlet_id, ns->switch_id, port_id);
        }
    } else {
        qv.erase(qv.begin() + idx);
    }

    ns->enqueued_mbit -= m->rc_accepted_mbit;
}

static void rollback_switch_egress(switch_state* ns, fluid_msg* m) {
    int port_id = m->port_id;

    if (port_id < 0 || port_id >= ns->num_ports || ns->queues[port_id] == NULL) {
        tw_error(TW_LOC, "invalid rollback egress port %d on switch %d", port_id, ns->switch_id);
    }

    ns->scheduled_egress_interval[port_id] = m->rc_prev_scheduled_egress_interval;
    ns->rr_next_queue_index[port_id] = m->rc_prev_rr_next_queue_index;

    std::vector<queued_flowlet>& qv = *ns->queues[port_id];
    const port_desc* p = &ns->ports[port_id];

    for (int r = m->rc_alloc_count - 1; r >= 0; --r) {
        rc_alloc_record* rc = &m->rc_allocs[r];

        if (!rc->valid || rc->send_mbit <= EPS) {
            continue;
        }

        int idx = rc->queue_index;

        if (idx < 0 || idx >= (int)qv.size() || qv[idx].flowlet_id != rc->before.flowlet_id) {
            idx = find_queue_index_for_flowlet(ns, port_id, rc->before);
        }

        if (idx >= 0) {
            qv[idx] = rc->before;
        } else {
            int insert_idx = rc->queue_index;

            if (insert_idx < 0) {
                insert_idx = 0;
            }
            if (insert_idx > (int)qv.size()) {
                insert_idx = (int)qv.size();
            }

            qv.insert(qv.begin() + insert_idx, rc->before);
        }

        ns->sent_mbit -= rc->send_mbit;
        ns->sent_fragments--;

        if (p->is_terminal) {
            ns->delivered_local_mbit -= rc->send_mbit;
        }
    }
}

static void switch_rev_event(switch_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;
    (void)lp;

    switch (m->event_type) {
    case FLOWLET_ARRIVAL:
        rollback_switch_arrival(ns, m);
        break;

    case SWITCH_EGRESS:
        rollback_switch_egress(ns, m);
        break;

    default:
        tw_error(TW_LOC, "switch reverse received unknown event type %d", m->event_type);
    }

    restore_switch_traffic_metrics_event(ns, m);
}

static void switch_commit_event(switch_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;
    (void)lp;

    if (!fluid_csv_commit_logs_enabled()) {
        return;
    }

    fluid_commit_logging_begin();

    switch (m->event_type) {
    case FLOWLET_ARRIVAL:
        log_switch_arrival_event(ns, m);
        break;

    case SWITCH_EGRESS:
        log_switch_egress_event(ns, m);
        break;

    default:
        break;
    }

    fluid_commit_logging_end();
}

static void switch_finalize(switch_state* ns, tw_lp* lp) {
    if (cfg.debug_prints) {
        double queued = 0.0;
        for (int p = 0; p < ns->num_ports; ++p) {
            queued += queued_mbit_on_port(ns, p);
        }

        printf("fluid-flow-wan gid=%llu switch=%d name=%s ports=%d "
               "shared_buffer_mbit=%.6f received_fragments=%d "
               "sent_fragments=%d enqueued_mbit=%.6f sent_mbit=%.6f "
               "local_delivery_mbit=%.6f dropped_mbit=%.6f "
               "queued_mbit=%.6f\n",
               (unsigned long long)lp->gid, ns->switch_id,
               switches[ns->switch_id].name.c_str(), ns->num_ports,
               ns->shared_buffer_mbit, ns->received_fragments,
               ns->sent_fragments, ns->enqueued_mbit, ns->sent_mbit,
               ns->delivered_local_mbit, ns->dropped_mbit, queued);
    }

    for (int p = 0; p < ns->num_ports; ++p) {
        delete ns->queues[p];
        ns->queues[p] = NULL;
    }
}

const tw_optdef app_opt[] = {TWOPT_GROUP("fluid-flow WAN switch/terminal workload"), TWOPT_END()};

static const char* find_config_arg(int argc, char** argv) {
    for (int i = argc - 1; i >= 1; --i) {
        if (argv[i] == NULL) {
            continue;
        }
        const char* arg = argv[i];
        size_t len = strlen(arg);
        if (len >= 5 && strcmp(arg + len - 5, ".conf") == 0) {
            return arg;
        }
    }
    return NULL;
}

static void write_log_header_file(const char* path, const char* header) {
    if (path[0] == '\0') {
        return;
    }

    std::remove(path);

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        tw_error(TW_LOC, "could not open CSV log file '%s' for header", path);
    }
    out << header;
}

static void append_merged_rows_to_file(const char* path, const char* data, int len) {
    if (path[0] == '\0' || len <= 0) {
        return;
    }

    std::ofstream out(path, std::ios::app);
    if (!out) {
        tw_error(TW_LOC, "could not open CSV log file '%s' for merged append", path);
    }
    out.write(data, len);
}

static void merge_log_buffer(const char* path, const std::ostringstream& buffer, MPI_Comm comm) {
    if (path[0] == '\0') {
        return;
    }

    int rank = 0;
    int size = 1;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &size);

    std::string local_rows = buffer.str();
    int local_len = (int)local_rows.size();

    std::vector<int> recv_counts;
    std::vector<int> displs;
    if (rank == 0) {
        recv_counts.resize(size, 0);
        displs.resize(size, 0);
    }

    MPI_Gather(&local_len, 1, MPI_INT, rank == 0 ? recv_counts.data() : NULL, 1, MPI_INT, 0, comm);

    int total_len = 0;
    if (rank == 0) {
        for (int i = 0; i < size; ++i) {
            displs[i] = total_len;
            total_len += recv_counts[i];
        }
    }

    std::vector<char> merged;
    if (rank == 0 && total_len > 0) {
        merged.resize(total_len);
    }

    MPI_Gatherv(local_len > 0 ? local_rows.data() : NULL, local_len, MPI_CHAR,
                rank == 0 && total_len > 0 ? merged.data() : NULL,
                rank == 0 ? recv_counts.data() : NULL, rank == 0 ? displs.data() : NULL, MPI_CHAR,
                0, comm);

    if (rank == 0 && total_len > 0) {
        append_merged_rows_to_file(path, merged.data(), total_len);
    }
}

static void merge_all_log_buffers(MPI_Comm comm) {
    merge_log_buffer(cfg.terminal_log_path, terminal_log_buffer, comm);
    merge_log_buffer(cfg.switch_log_path, switch_log_buffer, comm);
    merge_log_buffer(cfg.flowlet_log_path, flowlet_log_buffer, comm);
    merge_log_buffer(cfg.switch_training_log_path, switch_training_log_buffer, comm);
}

static void write_log_headers(int rank) {
    if (rank != 0) {
        return;
    }

    write_log_header_file(
        cfg.terminal_log_path,
        "interval,event,terminal,terminal_name,attached_switch,peer_terminal,mbit\n");

    write_log_header_file(cfg.switch_log_path,
                          "interval,event,switch,switch_name,port,target_type,target_index,"
                          "capacity_mbit,queued_before_mbit,sent_mbit,queued_after_mbit,"
                          "shared_queued_before_mbit,shared_queued_after_mbit,"
                          "shared_buffer_mbit,dropped_mbit,active_queue_entries\n");

    write_log_header_file(cfg.flowlet_log_path,
                          "interval,event,switch,switch_name,port,target_type,target_index,"
                          "flowlet_id,source_terminal,destination_terminal,creation_interval,"
                          "age_intervals,capacity_mbit,queued_before_mbit,send_mbit,"
                          "remaining_after_mbit,dropped_mbit\n");

    write_log_header_file(
        cfg.switch_training_log_path,
        "decision_id,interval,switch,switch_name,port,target_type,target_index,"
        "target_is_terminal,scheduler,candidate_selection_policy,used_fallback,"
        "total_queued_flowlets,candidate_count,candidate_rank,flowlet_id,"
        "source_terminal,destination_terminal,creation_interval,enqueue_interval,"
        "age_intervals,enqueue_age_intervals,remaining_mbit,allocation_mbit,"
        "capacity_mbit,port_queued_before_mbit,port_queued_after_mbit,"
        "shared_queued_before_mbit,shared_queued_after_mbit,shared_buffer_mbit,"
        "shared_buffer_occupancy,sent_total_mbit,ingress_mbit_current,"
        "ingress_mbit_previous,egress_mbit_current,egress_mbit_previous,"
        "dropped_mbit_current,dropped_mbit_previous,total_active_flowlets,"
        "active_before,active_after\n");
}

int main(int argc, char** argv) {
    int rank = 0;

    g_tw_ts_end = seconds_to_ns(60.0 * 60.0 * 24.0 * 365.0);

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    const char* config_file = find_config_arg(argc, argv);
    if (config_file == NULL) {
        printf("Usage: mpirun -np <n> %s --synch=1 -- <config.conf>\n", argv[0]);
        MPI_Finalize();
        return 1;
    }

    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    configuration_load(config_file, MPI_COMM_CODES, &config);
    load_config();
    validate_ross_message_size_or_abort(rank);
    configure_fluid_flow_wan_server_debug(rank);
    MPI_Barrier(MPI_COMM_CODES);

    add_lp_types();
    codes_mapping_setup();

    total_switch_lps = codes_mapping_get_lp_count(GROUP_NAME, 0, SWITCH_LP_NAME, NULL, 1);
    total_terminal_lps = codes_mapping_get_lp_count(GROUP_NAME, 0, TERMINAL_LP_NAME, NULL, 1);
    if (total_switch_lps != (int)switches.size()) {
        tw_error(TW_LOC, "config defines %d switch LPs but topology YAML defines %zu switches",
                 total_switch_lps, switches.size());
    }
    if (total_terminal_lps != (int)terminals.size()) {
        tw_error(TW_LOC, "config defines %d terminal LPs but topology YAML defines %zu terminals",
                 total_terminal_lps, terminals.size());
    }

    write_log_headers(rank);
    MPI_Barrier(MPI_COMM_CODES);

    if (rank == 0) {
        printf("fluid-flow-wan config: switches=%zu terminals=%zu interval_seconds=%.6f "
               "num_send_intervals=%d num_drain_intervals=%d switch_scheduler=%s "
               "director_candidate_policy=%s director_max_candidates=%d "
               "director_fallback=%s "
               "buffer_mode=shared csv_logs=%s ross_message_size=%d fluid_msg_size=%zu\n",
               switches.size(), terminals.size(), cfg.interval_seconds, cfg.num_send_intervals,
               cfg.num_drain_intervals, cfg.switch_scheduler,
               cfg.director_candidate_selection_policy, cfg.director_max_candidate_flowlets,
               cfg.director_fallback_scheduler,
               fluid_csv_forward_logs_enabled() ? "buffered-forward" : "buffered-commit",
               get_configured_message_size_bytes(), sizeof(fluid_msg));
    }

    tw_run();
    merge_all_log_buffers(MPI_COMM_CODES);
    tw_end();
    return 0;
}
