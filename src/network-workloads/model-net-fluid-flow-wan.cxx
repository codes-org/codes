
/*
 * Standalone interval-fluid switch/terminal workload model.
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
#include <fstream>
#include <limits>
#include <map>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include <ross.h>

#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"

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
static constexpr int MAX_PAUSE_INGRESS_LINKS = 128;
static constexpr double EPS = 1e-9;

static constexpr double PHASE_EARLY_SWITCH_EGRESS = 0.05;
static constexpr double PHASE_GENERATE = 0.10;
static constexpr double PHASE_ARRIVAL = 0.20;
static constexpr double PHASE_LATE_SWITCH_EGRESS = 0.60;
static constexpr double PHASE_ETHERNET_PAUSE_EVAL = 0.70;
static constexpr double PHASE_ETHERNET_PAUSE_FRAME_UPDATE = 0.75;

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
    double pause_high_watermark_fraction = 0.80;
    double pause_low_watermark_fraction = 0.50;
    int pause_duration_intervals = 2;
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
    int ingress_id;
    double remaining_mbit;
};

struct ingress_desc {
    int is_terminal;
    int peer_index;
    double queued_mbit;
    int pause_asserted;
    int last_pause_frame_interval;
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
    int link_paused_until_interval;
    unsigned long long pause_updates_received;
    unsigned long long pause_frames_received;
    unsigned long long resume_frames_received;
    unsigned long long link_paused_intervals;
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
     * Rollback-safe demand-driven egress scheduling.  Early and late egress
     * events are tracked independently for every port and interval so Time
     * Warp may have multiple future intervals outstanding at once.
     */
    std::vector<unsigned char>* scheduled_early_egress[MAX_PORTS_PER_SWITCH];
    std::vector<unsigned char>* scheduled_late_egress[MAX_PORTS_PER_SWITCH];
    int capacity_accounting_interval[MAX_PORTS_PER_SWITCH];
    double capacity_used_mbit[MAX_PORTS_PER_SWITCH];
    int output_link_paused_until_interval[MAX_PORTS_PER_SWITCH];

    int num_ingress_links;
    ingress_desc ingress_links[MAX_PAUSE_INGRESS_LINKS];
    double pause_high_watermark_mbit;
    double pause_low_watermark_mbit;
    unsigned long long pause_updates_sent;
    unsigned long long pause_frames_sent;
    unsigned long long resume_frames_sent;
    unsigned long long pause_updates_received;
    unsigned long long pause_frames_received;
    unsigned long long resume_frames_received;
    unsigned long long paused_egress_intervals;

    double admitted_arrival_mbit;
    double sent_mbit;
    double delivered_local_mbit;
    double dropped_mbit;
    int received_fragments;
    int sent_fragments;
};

enum fluid_event_type {
    WORKLOAD_GENERATE = 1,
    FLOWLET_ARRIVAL = 2,
    SWITCH_EGRESS_EARLY = 3,
    ETHERNET_PAUSE_EVAL = 4,
    SWITCH_EGRESS_LATE = 5,
    ETHERNET_PAUSE_FRAME_UPDATE = 6,
};

struct rc_alloc_record {
    int valid;
    int queue_index;
    queued_flowlet before;
    double send_mbit;
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
    int pause_asserted;
    int pause_source_switch;

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
    int rc_pause_state_changed;
    int rc_prev_pause_until_interval;
    int rc_pause_target_port;
    int rc_ingress_id;
    int rc_pause_change_count;
    int rc_pause_ingress_id[MAX_PAUSE_INGRESS_LINKS];
    int rc_pause_prev_asserted[MAX_PAUSE_INGRESS_LINKS];
    int rc_pause_prev_last_interval[MAX_PAUSE_INGRESS_LINKS];
    int rc_pause_sent_asserted[MAX_PAUSE_INGRESS_LINKS];

    int rc_egress_event_active;
    int rc_egress_request_created;
    int rc_egress_request_event_type;
    int rc_egress_request_interval;
    int rc_egress_request_port;
    int rc_prev_capacity_accounting_interval;
    double rc_prev_capacity_used_mbit;

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

    rc_alloc_record rc_allocs[MAX_RC_ALLOCATIONS];
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
    read_double_param("FLUID_FLOW_WAN", "pause_high_watermark_fraction",
                      &cfg.pause_high_watermark_fraction);
    read_double_param("FLUID_FLOW_WAN", "pause_low_watermark_fraction",
                      &cfg.pause_low_watermark_fraction);
    read_int_param("FLUID_FLOW_WAN", "pause_duration_intervals",
                   &cfg.pause_duration_intervals);
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

    if (cfg.pause_duration_intervals <= 0) {
        tw_error(TW_LOC, "pause_duration_intervals must be positive, got %d",
                 cfg.pause_duration_intervals);
    }

    if (!(cfg.pause_low_watermark_fraction >= 0.0 &&
          cfg.pause_low_watermark_fraction < cfg.pause_high_watermark_fraction &&
          cfg.pause_high_watermark_fraction <= 1.0)) {
        tw_error(TW_LOC,
                 "PAUSE watermarks must satisfy 0 <= low < high <= 1; got low=%.6f high=%.6f",
                 cfg.pause_low_watermark_fraction, cfg.pause_high_watermark_fraction);
    }

    if (cfg.topology_yaml_file[0] == '\0') {
        tw_error(TW_LOC, "FLUID_FLOW_WAN.topology_yaml_file is required");
    }
    load_topology_yaml(cfg.topology_yaml_file);
    compute_routes();
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
            qv[i].creation_interval == m->creation_interval &&
            qv[i].ingress_id == m->rc_ingress_id) {
            qv[i].remaining_mbit += accepted;
            qv[i].age_intervals = m->interval_id - m->creation_interval;
            ns->admitted_arrival_mbit += accepted;

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
    q.ingress_id = m->rc_ingress_id;
    q.remaining_mbit = accepted;

    qv.push_back(q);
    if (queue_index_out != NULL) {
        *queue_index_out = (int)qv.size() - 1;
    }
    ns->admitted_arrival_mbit += accepted;

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
            qv[i].creation_interval == needle.creation_interval &&
            qv[i].ingress_id == needle.ingress_id) {
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
            qv[i].creation_interval == m->creation_interval &&
            qv[i].ingress_id == m->rc_ingress_id) {
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

static void append_switch_training_log(int interval_id, int switch_id, int port_id,
                                       int target_is_terminal, int target_index,
                                       double capacity_mbit, double queued_before_mbit,
                                       double sent_mbit, double queued_after_mbit,
                                       double dropped_mbit, int active_before, int active_after,
                                       const std::vector<std::string>& allocations) {
    std::ostringstream row;
    row << interval_id << ',' << switch_id << ',' << switches[switch_id].name << ',' << port_id
        << ',' << (target_is_terminal ? "terminal" : "switch") << ',' << target_index << ','
        << capacity_mbit << ',' << queued_before_mbit << ','
        << sent_mbit << ',' << queued_after_mbit << ',' << dropped_mbit << ',' << active_before
        << ',' << active_after << ',';
    for (size_t i = 0; i < allocations.size(); ++i) {
        if (i > 0) {
            row << ';';
        }
        row << allocations[i];
    }
    row << '\n';
    append_log_row(cfg.switch_training_log_path, &switch_training_log_buffer, row.str());
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

static void schedule_switch_egress(int event_type, int interval_id, int port_id, tw_lp* lp) {
    if (interval_id >= cfg.num_send_intervals + cfg.num_drain_intervals) {
        return;
    }

    double phase = 0.0;
    if (event_type == SWITCH_EGRESS_EARLY) {
        phase = PHASE_EARLY_SWITCH_EGRESS;
    } else if (event_type == SWITCH_EGRESS_LATE) {
        phase = PHASE_LATE_SWITCH_EGRESS;
    } else {
        tw_error(TW_LOC, "invalid switch egress event type %d", event_type);
    }

    tw_event* e = tw_event_new(lp->gid, delay_until_ns(interval_id, phase, lp), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = event_type;
    m->interval_id = interval_id;
    m->port_id = port_id;
    tw_event_send(e);
}

static std::vector<unsigned char>* scheduled_egress_flags(switch_state* ns,
                                                          int event_type,
                                                          int port_id) {
    if (event_type == SWITCH_EGRESS_EARLY) {
        return ns->scheduled_early_egress[port_id];
    }
    if (event_type == SWITCH_EGRESS_LATE) {
        return ns->scheduled_late_egress[port_id];
    }
    tw_error(TW_LOC, "invalid switch egress event type %d", event_type);
    return NULL;
}

static void undo_requested_switch_egress(switch_state* ns, fluid_msg* m) {
    if (!m->rc_egress_request_created) {
        return;
    }

    std::vector<unsigned char>* flags = scheduled_egress_flags(
        ns, m->rc_egress_request_event_type, m->rc_egress_request_port);
    int interval_id = m->rc_egress_request_interval;
    if (flags == NULL || interval_id < 0 || interval_id >= (int)flags->size()) {
        tw_error(TW_LOC, "invalid rollback egress request: switch %d port %d interval %d",
                 ns->switch_id, m->rc_egress_request_port, interval_id);
    }
    if (!(*flags)[interval_id]) {
        tw_error(TW_LOC,
                 "rollback expected scheduled egress flag: switch %d port %d interval %d",
                 ns->switch_id, m->rc_egress_request_port, interval_id);
    }
    (*flags)[interval_id] = 0;
}

static void request_switch_egress(switch_state* ns, int event_type, int interval_id,
                                  int port_id, tw_lp* lp, fluid_msg* cause_msg) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid request_switch_egress port %d on switch %d", port_id,
                 ns->switch_id);
    }

    int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    if (interval_id < 0 || interval_id >= total_intervals) {
        return;
    }

    std::vector<unsigned char>* flags = scheduled_egress_flags(ns, event_type, port_id);
    if (flags == NULL || interval_id >= (int)flags->size()) {
        tw_error(TW_LOC, "egress scheduling interval %d out of range on switch %d port %d",
                 interval_id, ns->switch_id, port_id);
    }
    if ((*flags)[interval_id]) {
        return;
    }

    if (cause_msg != NULL && cause_msg->rc_egress_request_created) {
        tw_error(TW_LOC,
                 "event attempted to create more than one egress request on switch %d",
                 ns->switch_id);
    }

    (*flags)[interval_id] = 1;
    schedule_switch_egress(event_type, interval_id, port_id, lp);

    if (cause_msg != NULL) {
        cause_msg->rc_egress_request_created = 1;
        cause_msg->rc_egress_request_event_type = event_type;
        cause_msg->rc_egress_request_interval = interval_id;
        cause_msg->rc_egress_request_port = port_id;
    }
}

static void schedule_pause_update(int interval_id, tw_lpid dst_gid, int pause_asserted,
                                  int pause_source_switch, tw_lp* lp) {
    tw_event* e = tw_event_new(
        dst_gid, delay_until_ns(interval_id, PHASE_ETHERNET_PAUSE_FRAME_UPDATE, lp), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = ETHERNET_PAUSE_FRAME_UPDATE;
    m->interval_id = interval_id;
    m->pause_asserted = pause_asserted;
    m->pause_source_switch = pause_source_switch;
    tw_event_send(e);
}

static bool switch_has_directed_link_to(int src_switch, int dst_switch) {
    const switch_info& sw = switches[src_switch];
    for (size_t i = 0; i < sw.links.size(); ++i) {
        if (sw.links[i].dst_switch == dst_switch) {
            return true;
        }
    }
    return false;
}

static int find_ingress_link(const switch_state* ns, int is_terminal, int peer_index) {
    for (int i = 0; i < ns->num_ingress_links; ++i) {
        const ingress_desc& ingress = ns->ingress_links[i];
        if (ingress.is_terminal == is_terminal && ingress.peer_index == peer_index) {
            return i;
        }
    }
    return -1;
}

static void send_pause_update_for_ingress(switch_state* ns, int ingress_id, int interval_id,
                                          int pause_asserted, tw_lp* lp) {
    if (ingress_id < 0 || ingress_id >= ns->num_ingress_links) {
        tw_error(TW_LOC, "invalid ingress id %d on switch %d", ingress_id, ns->switch_id);
    }
    const ingress_desc& ingress = ns->ingress_links[ingress_id];
    tw_lpid dst_gid = ingress.is_terminal ? get_terminal_gid(ingress.peer_index)
                                          : get_switch_gid(ingress.peer_index);
    schedule_pause_update(interval_id, dst_gid, pause_asserted, ns->switch_id, lp);
    ns->pause_updates_sent++;
    if (pause_asserted) {
        ns->pause_frames_sent++;
    } else {
        ns->resume_frames_sent++;
    }
}

static void schedule_ethernet_pause_eval(int interval_id, tw_lp* lp) {
    int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    if (interval_id < 0 || interval_id >= total_intervals) {
        return;
    }

    tw_event* e = tw_event_new(
        lp->gid, delay_until_ns(interval_id, PHASE_ETHERNET_PAUSE_EVAL, lp), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = ETHERNET_PAUSE_EVAL;
    m->interval_id = interval_id;
    tw_event_send(e);
}

static void record_pause_change(fluid_msg* m, int ingress_id, const ingress_desc& ingress,
                                int sent_asserted) {
    if (m->rc_pause_change_count >= MAX_PAUSE_INGRESS_LINKS) {
        tw_error(TW_LOC, "too many PAUSE ingress changes in one event");
    }
    int i = m->rc_pause_change_count++;
    m->rc_pause_ingress_id[i] = ingress_id;
    m->rc_pause_prev_asserted[i] = ingress.pause_asserted;
    m->rc_pause_prev_last_interval[i] = ingress.last_pause_frame_interval;
    m->rc_pause_sent_asserted[i] = sent_asserted;
}

static void handle_ethernet_pause_eval(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    m->rc_pause_change_count = 0;

    const double shared_occupancy_mbit = queued_mbit_on_switch(ns);

    /*
     * The physical buffer is shared. PAUSE decisions therefore use global
     * high/low watermarks, while per-ingress queued_mbit is used only to choose
     * which directly connected senders should be throttled.
     *
     * Hysteresis policy:
     *   - at or below the low watermark, resume every paused ingress;
     *   - at or above the high watermark, keep already-paused ingresses and
     *     pause the largest unpaused contributors until their combined queued
     *     traffic covers the occupancy above the high watermark;
     *   - between the watermarks, preserve the current PAUSE set;
     *   - refresh each asserted PAUSE only when its configured timer expires.
     *
     * Contributor ordering is deterministic: descending queued bytes, then
     * ascending ingress id. This is required for sequential/optimistic parity.
     */
    if (shared_occupancy_mbit <= ns->pause_low_watermark_mbit + EPS) {
        for (int ingress_id = 0; ingress_id < ns->num_ingress_links; ++ingress_id) {
            ingress_desc& ingress = ns->ingress_links[ingress_id];
            if (!ingress.pause_asserted) {
                continue;
            }

            record_pause_change(m, ingress_id, ingress, 0);
            ingress.pause_asserted = 0;
            ingress.last_pause_frame_interval = -1;
            send_pause_update_for_ingress(ns, ingress_id, m->interval_id, 0, lp);
        }
    } else {
        if (shared_occupancy_mbit + EPS >= ns->pause_high_watermark_mbit) {
            double covered_mbit = 0.0;
            bool has_paused_ingress = false;
            std::vector<int> candidates;
            candidates.reserve(ns->num_ingress_links);

            for (int ingress_id = 0; ingress_id < ns->num_ingress_links; ++ingress_id) {
                const ingress_desc& ingress = ns->ingress_links[ingress_id];
                if (ingress.pause_asserted) {
                    has_paused_ingress = true;
                    covered_mbit += ingress.queued_mbit;
                } else if (ingress.queued_mbit > EPS) {
                    candidates.push_back(ingress_id);
                }
            }

            std::sort(candidates.begin(), candidates.end(), [ns](int lhs, int rhs) {
                const double lhs_mbit = ns->ingress_links[lhs].queued_mbit;
                const double rhs_mbit = ns->ingress_links[rhs].queued_mbit;
                if (std::fabs(lhs_mbit - rhs_mbit) > EPS) {
                    return lhs_mbit > rhs_mbit;
                }
                return lhs < rhs;
            });

            const double excess_mbit =
                shared_occupancy_mbit - ns->pause_high_watermark_mbit;
            for (int ingress_id : candidates) {
                if (has_paused_ingress && covered_mbit + EPS >= excess_mbit) {
                    break;
                }

                ingress_desc& ingress = ns->ingress_links[ingress_id];
                record_pause_change(m, ingress_id, ingress, 1);
                ingress.pause_asserted = 1;
                ingress.last_pause_frame_interval = m->interval_id;
                has_paused_ingress = true;
                covered_mbit += ingress.queued_mbit;
                send_pause_update_for_ingress(ns, ingress_id, m->interval_id, 1, lp);
            }
        }

        for (int ingress_id = 0; ingress_id < ns->num_ingress_links; ++ingress_id) {
            ingress_desc& ingress = ns->ingress_links[ingress_id];
            if (!ingress.pause_asserted ||
                m->interval_id <
                    ingress.last_pause_frame_interval + cfg.pause_duration_intervals) {
                continue;
            }

            record_pause_change(m, ingress_id, ingress, 1);
            ingress.last_pause_frame_interval = m->interval_id;
            send_pause_update_for_ingress(ns, ingress_id, m->interval_id, 1, lp);
        }
    }

    schedule_ethernet_pause_eval(m->interval_id + 1, lp);
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
    ns->link_paused_until_interval = -1;
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

static void build_allocation_strings_from_rc(const fluid_msg* m,
                                             std::vector<std::string>& allocations) {
    allocations.clear();

    for (int i = 0; i < m->rc_alloc_count; ++i) {
        const rc_alloc_record* rc = &m->rc_allocs[i];

        if (!rc->valid || rc->send_mbit <= EPS) {
            continue;
        }

        double remaining_after = rc->before.remaining_mbit - rc->send_mbit;
        if (remaining_after < 0.0 && remaining_after > -EPS) {
            remaining_after = 0.0;
        }

        std::ostringstream ss;
        ss << rc->before.flowlet_id << ':' << rc->before.source_terminal << ':'
           << rc->before.destination_terminal << ':' << rc->before.creation_interval << ':'
           << rc->before.remaining_mbit << ':' << rc->send_mbit << ':' << remaining_after;
        allocations.push_back(ss.str());
    }
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

    std::vector<std::string> allocations;
    build_allocation_strings_from_rc(m, allocations);
    append_switch_training_log(m->interval_id, ns->switch_id, m->port_id,
                               m->rc_log_target_is_terminal, m->rc_log_target_index,
                               m->rc_log_capacity_mbit, m->rc_log_port_queued_before_mbit,
                               m->rc_log_sent_total_mbit, m->rc_log_port_queued_after_mbit, 0.0,
                               m->rc_log_active_before_entries, m->rc_log_active_after_entries,
                               allocations);
}

static void handle_workload_generate(terminal_state* ns, fluid_msg* m, tw_lp* lp) {
    int interval = m->interval_id;

    m->rc_rng_count = 0;
    m->rc_generated = 0;
    m->mbit = 0.0;
    m->destination_terminal = -1;
    m->flowlet_id = 0;

    if (interval < ns->link_paused_until_interval) {
        ns->link_paused_intervals++;
        m->rc_pause_target_port = -2;
        schedule_workload_generate(ns, next_terminal_generate_interval_after(interval), lp);
        return;
    }

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
    case ETHERNET_PAUSE_FRAME_UPDATE:
        m->rc_prev_pause_until_interval = ns->link_paused_until_interval;
        if (m->pause_asserted) {
            ns->link_paused_until_interval =
                std::max(ns->link_paused_until_interval,
                         m->interval_id + cfg.pause_duration_intervals + 1);
        } else {
            ns->link_paused_until_interval = m->interval_id;
        }
        m->rc_pause_state_changed =
            (ns->link_paused_until_interval != m->rc_prev_pause_until_interval);
        ns->pause_updates_received++;
        if (m->pause_asserted) {
            ns->pause_frames_received++;
        } else {
            ns->resume_frames_received++;
        }
        break;
    default:
        tw_error(TW_LOC, "terminal received unknown event type %d", m->event_type);
    }
}

static void terminal_rev_event(terminal_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;

    switch (m->event_type) {
    case WORKLOAD_GENERATE:
        if (m->rc_pause_target_port == -2) {
            ns->link_paused_intervals--;
        }
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

    case ETHERNET_PAUSE_FRAME_UPDATE:
        ns->link_paused_until_interval = m->rc_prev_pause_until_interval;
        ns->pause_updates_received--;
        if (m->pause_asserted) {
            ns->pause_frames_received--;
        } else {
            ns->resume_frames_received--;
        }
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
    printf(
        "fluid-flow-wan-terminal gid=%llu terminal=%d switch=%d generated_mbit=%.6f sent_mbit=%.6f "
        "received_mbit=%.6f pause_updates_received=%llu pause_frames_received=%llu "
        "resume_frames_received=%llu link_paused_intervals=%llu generated_flowlets=%d "
        "received_fragments=%d\n",
        (unsigned long long)lp->gid, ns->terminal_id, ns->attached_switch, ns->generated_mbit,
        ns->sent_to_switch_mbit, ns->received_mbit, ns->pause_updates_received,
        ns->pause_frames_received, ns->resume_frames_received, ns->link_paused_intervals,
        ns->generated_flowlets, ns->received_fragments);
}

static void switch_init(switch_state* ns, tw_lp* lp) {
    memset(ns, 0, sizeof(*ns));
    ns->switch_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    if (ns->switch_id < 0 || ns->switch_id >= (int)switches.size()) {
        tw_error(TW_LOC, "switch LP relative id %d out of range", ns->switch_id);
    }

    int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    for (int p = 0; p < MAX_PORTS_PER_SWITCH; ++p) {
        ns->scheduled_early_egress[p] =
            new std::vector<unsigned char>(total_intervals, 0);
        ns->scheduled_late_egress[p] =
            new std::vector<unsigned char>(total_intervals, 0);
        ns->capacity_accounting_interval[p] = -1;
        ns->capacity_used_mbit[p] = 0.0;
        ns->output_link_paused_until_interval[p] = -1;
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
    ns->num_ingress_links = 0;

    for (int t = 0; t < sw.terminal_count; ++t) {
        ingress_desc& ingress = ns->ingress_links[ns->num_ingress_links++];
        ingress.is_terminal = 1;
        ingress.peer_index = sw.terminal_start + t;
        ingress.queued_mbit = 0.0;
        ingress.pause_asserted = 0;
        ingress.last_pause_frame_interval = -1;
    }
    for (int upstream = 0; upstream < (int)switches.size(); ++upstream) {
        if (upstream == ns->switch_id ||
            !switch_has_directed_link_to(upstream, ns->switch_id)) {
            continue;
        }
        if (ns->num_ingress_links >= MAX_PAUSE_INGRESS_LINKS) {
            tw_error(TW_LOC, "too many ingress links on switch %d", ns->switch_id);
        }
        ingress_desc& ingress = ns->ingress_links[ns->num_ingress_links++];
        ingress.is_terminal = 0;
        ingress.peer_index = upstream;
        ingress.queued_mbit = 0.0;
        ingress.pause_asserted = 0;
        ingress.last_pause_frame_interval = -1;
    }
    if (ns->num_ingress_links <= 0) {
        tw_error(TW_LOC, "switch %d has no ingress links", ns->switch_id);
    }
    ns->pause_high_watermark_mbit =
        ns->shared_buffer_mbit * cfg.pause_high_watermark_fraction;
    ns->pause_low_watermark_mbit =
        ns->shared_buffer_mbit * cfg.pause_low_watermark_fraction;

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
     *
     * PAUSE hysteresis is evaluated exactly once per switch and interval by a
     * periodic ETHERNET_PAUSE_EVAL event.
     */
    schedule_ethernet_pause_eval(0, lp);
}


static void handle_switch_arrival(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    m->rc_egress_request_created = 0;
    ns->received_fragments++;

    int ingress_id = -1;
    if (m->source_switch == ns->switch_id) {
        ingress_id = find_ingress_link(ns, 1, m->source_terminal);
    } else {
        ingress_id = find_ingress_link(ns, 0, m->source_switch);
    }
    if (ingress_id < 0) {
        tw_error(TW_LOC, "switch %d could not identify ingress for source terminal %d switch %d",
                 ns->switch_id, m->source_terminal, m->source_switch);
    }
    m->rc_ingress_id = ingress_id;

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
    ns->ingress_links[ingress_id].queued_mbit += accepted;

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
        request_switch_egress(ns, SWITCH_EGRESS_LATE, m->interval_id, port_id, lp, m);
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

static void handle_switch_egress(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    m->rc_egress_request_created = 0;
    int port_id = m->port_id;
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid switch egress port %d", port_id);
    }

    std::vector<unsigned char>* scheduled =
        scheduled_egress_flags(ns, m->event_type, port_id);
    m->rc_egress_event_active =
        scheduled != NULL && m->interval_id >= 0 &&
        m->interval_id < (int)scheduled->size() && (*scheduled)[m->interval_id];

    /* A duplicate or canceled event is a forward and reverse no-op. */
    if (!m->rc_egress_event_active) {
        return;
    }

    (*scheduled)[m->interval_id] = 0;

    if (ns->queues[port_id] == NULL) {
        m->rc_prev_capacity_accounting_interval =
            ns->capacity_accounting_interval[port_id];
        m->rc_prev_capacity_used_mbit = ns->capacity_used_mbit[port_id];
        return;
    }

    port_desc* p = &ns->ports[port_id];
    std::vector<queued_flowlet>& qv = *ns->queues[port_id];

    m->rc_prev_capacity_accounting_interval = ns->capacity_accounting_interval[port_id];
    m->rc_prev_capacity_used_mbit = ns->capacity_used_mbit[port_id];

    if (ns->capacity_accounting_interval[port_id] != m->interval_id) {
        ns->capacity_accounting_interval[port_id] = m->interval_id;
        ns->capacity_used_mbit[port_id] = 0.0;
    }

    double capacity = p->capacity_mbit_per_interval;
    double remaining_capacity =
        std::max(0.0, capacity - ns->capacity_used_mbit[port_id]);
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
    m->rc_pause_state_changed = 0;
    m->rc_pause_target_port = -1;

    if (m->interval_id < ns->output_link_paused_until_interval[port_id]) {
        ns->paused_egress_intervals++;
        m->rc_pause_target_port = port_id;
        log_switch_egress_event(ns, m);
        if (queued_before > EPS) {
            request_switch_egress(ns, SWITCH_EGRESS_EARLY, m->interval_id + 1, port_id, lp, m);
        }
        return;
    }

    auto add_to_plan = [&](int idx, double requested_send) -> double {
        if (idx < 0 || idx >= (int)send_plan.size() || requested_send <= EPS) {
            return 0.0;
        }

        double still_available_for_flowlet = qv[idx].remaining_mbit - send_plan[idx];
        if (still_available_for_flowlet <= EPS) {
            return 0.0;
        }

        double actual_send = std::min(requested_send, still_available_for_flowlet);
        send_plan[idx] += actual_send;
        return actual_send;
    };

    /*
     * Deterministic max-min fair sharing. Every active flowlet on an
     * unpaused output link receives an equal share of the interval capacity.
     * If a flowlet needs less than its share, the unused capacity is
     * redistributed among the remaining flowlets.
     */
    if (active_before > MAX_RC_ALLOCATIONS) {
        tw_error(TW_LOC,
                 "fair-share egress on switch %d port %d has %d active flowlets, "
                 "exceeding MAX_RC_ALLOCATIONS=%d",
                 ns->switch_id, port_id, active_before, MAX_RC_ALLOCATIONS);
    }

    while (remaining_capacity > EPS) {
        int unsatisfied_count = 0;
        for (int i = 0; i < (int)qv.size(); ++i) {
            if (qv[i].valid && qv[i].remaining_mbit - send_plan[i] > EPS) {
                unsatisfied_count++;
            }
        }

        if (unsatisfied_count == 0) {
            break;
        }

        const double equal_share = remaining_capacity / unsatisfied_count;
        double allocated_this_round = 0.0;

        for (int i = 0; i < (int)qv.size(); ++i) {
            if (!qv[i].valid || qv[i].remaining_mbit - send_plan[i] <= EPS) {
                continue;
            }

            const double planned_send = add_to_plan(i, equal_share);
            allocated_this_round += planned_send;
        }

        if (allocated_this_round <= EPS) {
            break;
        }

        remaining_capacity -= allocated_this_round;
        if (remaining_capacity < 0.0 && remaining_capacity > -EPS) {
            remaining_capacity = 0.0;
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
        if (before.ingress_id < 0 || before.ingress_id >= ns->num_ingress_links) {
            tw_error(TW_LOC, "invalid ingress id %d on queued flowlet", before.ingress_id);
        }
        ns->ingress_links[before.ingress_id].queued_mbit -= send;
        if (ns->ingress_links[before.ingress_id].queued_mbit < 0.0 &&
            ns->ingress_links[before.ingress_id].queued_mbit > -EPS) {
            ns->ingress_links[before.ingress_id].queued_mbit = 0.0;
        }
        sent_total += send;
    }

    ns->capacity_used_mbit[port_id] += sent_total;
    if (ns->capacity_used_mbit[port_id] > capacity + EPS) {
        tw_error(TW_LOC,
                 "switch %d port %d exceeded interval capacity: used %.12f capacity %.12f",
                 ns->switch_id, port_id, ns->capacity_used_mbit[port_id], capacity);
    }

    compact_port_queue(ns, port_id);


    double queued_after = queued_mbit_on_port(ns, port_id);
    double shared_queued_after = queued_mbit_on_switch(ns);
    int active_after = active_flowlet_count_on_port(ns, port_id);

    m->rc_log_port_queued_after_mbit = queued_after;
    m->rc_log_shared_queued_after_mbit = shared_queued_after;
    m->rc_log_sent_total_mbit = sent_total;
    m->rc_log_active_after_entries = active_after;

    log_switch_egress_event(ns, m);

    if (queued_after > EPS) {
        request_switch_egress(ns, SWITCH_EGRESS_EARLY, m->interval_id + 1, port_id, lp, m);
    }
}


static void handle_switch_pause_update(switch_state* ns, fluid_msg* m) {
    int port_id = find_switch_link_port(ns, m->pause_source_switch);
    if (port_id < 0) {
        tw_error(TW_LOC,
                 "switch %d received PAUSE update from switch %d but has no output link to it",
                 ns->switch_id, m->pause_source_switch);
    }

    m->rc_pause_target_port = port_id;
    m->rc_prev_pause_until_interval = ns->output_link_paused_until_interval[port_id];
    if (m->pause_asserted) {
        ns->output_link_paused_until_interval[port_id] =
            std::max(ns->output_link_paused_until_interval[port_id],
                     m->interval_id + cfg.pause_duration_intervals + 1);
    } else {
        ns->output_link_paused_until_interval[port_id] = m->interval_id;
    }
    m->rc_pause_state_changed =
        (ns->output_link_paused_until_interval[port_id] !=
         m->rc_prev_pause_until_interval);
    ns->pause_updates_received++;
    if (m->pause_asserted) {
        ns->pause_frames_received++;
    } else {
        ns->resume_frames_received++;
    }
}

static void switch_event(switch_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;
    switch (m->event_type) {
    case FLOWLET_ARRIVAL:
        handle_switch_arrival(ns, m, lp);
        break;
    case SWITCH_EGRESS_EARLY:
    case SWITCH_EGRESS_LATE:
        handle_switch_egress(ns, m, lp);
        break;
    case ETHERNET_PAUSE_FRAME_UPDATE:
        handle_switch_pause_update(ns, m);
        break;
    case ETHERNET_PAUSE_EVAL:
        handle_ethernet_pause_eval(ns, m, lp);
        break;
    default:
        tw_error(TW_LOC, "switch received unknown event type %d", m->event_type);
    }
}

static void rollback_switch_arrival(switch_state* ns, fluid_msg* m) {
    ns->received_fragments--;

    undo_requested_switch_egress(ns, m);

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

    ns->admitted_arrival_mbit -= m->rc_accepted_mbit;
    ns->ingress_links[m->rc_ingress_id].queued_mbit -= m->rc_accepted_mbit;
    if (ns->ingress_links[m->rc_ingress_id].queued_mbit < 0.0 &&
        ns->ingress_links[m->rc_ingress_id].queued_mbit > -EPS) {
        ns->ingress_links[m->rc_ingress_id].queued_mbit = 0.0;
    }
}

static void rollback_switch_egress(switch_state* ns, fluid_msg* m) {
    if (!m->rc_egress_event_active) {
        return;
    }

    int port_id = m->port_id;

    if (port_id < 0 || port_id >= ns->num_ports || ns->queues[port_id] == NULL) {
        tw_error(TW_LOC, "invalid rollback egress port %d on switch %d", port_id, ns->switch_id);
    }

    undo_requested_switch_egress(ns, m);

    std::vector<unsigned char>* scheduled =
        scheduled_egress_flags(ns, m->event_type, port_id);
    if (scheduled == NULL || m->interval_id < 0 ||
        m->interval_id >= (int)scheduled->size()) {
        tw_error(TW_LOC, "invalid rollback egress interval %d on switch %d port %d",
                 m->interval_id, ns->switch_id, port_id);
    }
    (*scheduled)[m->interval_id] = 1;
    ns->capacity_accounting_interval[port_id] =
        m->rc_prev_capacity_accounting_interval;
    ns->capacity_used_mbit[port_id] = m->rc_prev_capacity_used_mbit;

    if (m->rc_pause_target_port == port_id && m->rc_alloc_count == 0) {
        ns->paused_egress_intervals--;
        return;
    }


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

        ns->ingress_links[rc->before.ingress_id].queued_mbit += rc->send_mbit;
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

    case SWITCH_EGRESS_EARLY:
    case SWITCH_EGRESS_LATE:
        rollback_switch_egress(ns, m);
        break;

    case ETHERNET_PAUSE_EVAL:
        for (int i = m->rc_pause_change_count - 1; i >= 0; --i) {
            int ingress_id = m->rc_pause_ingress_id[i];
            ingress_desc& ingress = ns->ingress_links[ingress_id];
            ingress.pause_asserted = m->rc_pause_prev_asserted[i];
            ingress.last_pause_frame_interval = m->rc_pause_prev_last_interval[i];
            ns->pause_updates_sent--;
            if (m->rc_pause_sent_asserted[i]) {
                ns->pause_frames_sent--;
            } else {
                ns->resume_frames_sent--;
            }
        }
        break;

    case ETHERNET_PAUSE_FRAME_UPDATE:
        ns->output_link_paused_until_interval[m->rc_pause_target_port] =
            m->rc_prev_pause_until_interval;
        ns->pause_updates_received--;
        if (m->pause_asserted) {
            ns->pause_frames_received--;
        } else {
            ns->resume_frames_received--;
        }
        break;

    default:
        tw_error(TW_LOC, "switch reverse received unknown event type %d", m->event_type);
    }
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

    case SWITCH_EGRESS_EARLY:
    case SWITCH_EGRESS_LATE:
        log_switch_egress_event(ns, m);
        break;

    default:
        break;
    }

    fluid_commit_logging_end();
}

static void switch_finalize(switch_state* ns, tw_lp* lp) {
    double queued = 0.0;
    int any_pause_asserted = 0;
    for (int i = 0; i < ns->num_ingress_links; ++i) {
        any_pause_asserted |= ns->ingress_links[i].pause_asserted;
    }
    for (int p = 0; p < ns->num_ports; ++p) {
        queued += queued_mbit_on_port(ns, p);
    }
    printf("fluid-flow-wan gid=%llu switch=%d name=%s ports=%d shared_buffer_mbit=%.6f "
           "received_fragments=%d sent_fragments=%d admitted_arrival_mbit=%.6f sent_mbit=%.6f "
           "local_delivery_mbit=%.6f dropped_mbit=%.6f ready_queue_mbit=%.6f "
           "shared_buffer_occupied_mbit=%.6f pause_asserted=%d pause_updates_sent=%llu "
           "pause_frames_sent=%llu resume_frames_sent=%llu pause_updates_received=%llu "
           "pause_frames_received=%llu resume_frames_received=%llu paused_egress_intervals=%llu\n",
           (unsigned long long)lp->gid, ns->switch_id, switches[ns->switch_id].name.c_str(),
           ns->num_ports, ns->shared_buffer_mbit, ns->received_fragments, ns->sent_fragments,
           ns->admitted_arrival_mbit, ns->sent_mbit, ns->delivered_local_mbit, ns->dropped_mbit,
           queued, queued, any_pause_asserted, ns->pause_updates_sent, ns->pause_frames_sent,
           ns->resume_frames_sent, ns->pause_updates_received, ns->pause_frames_received,
           ns->resume_frames_received, ns->paused_egress_intervals);

    for (int p = 0; p < ns->num_ports; ++p) {
        delete ns->queues[p];
        ns->queues[p] = NULL;
    }
    for (int p = 0; p < MAX_PORTS_PER_SWITCH; ++p) {
        delete ns->scheduled_early_egress[p];
        delete ns->scheduled_late_egress[p];
        ns->scheduled_early_egress[p] = NULL;
        ns->scheduled_late_egress[p] = NULL;
    }
}

const tw_optdef app_opt[] = {TWOPT_GROUP("interval-fluid switch/terminal workload"), TWOPT_END()};

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

    write_log_header_file(cfg.switch_training_log_path,
                          "interval,switch,switch_name,port,target_type,target_index,"
                          "capacity_mbit,queued_before_mbit,sent_mbit,queued_after_mbit,"
                          "dropped_mbit,active_before,active_after,allocations\n");
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
               "num_send_intervals=%d num_drain_intervals=%d "
               "buffer_mode=shared csv_logs=%s ross_message_size=%d fluid_msg_size=%zu\n",
               switches.size(), terminals.size(), cfg.interval_seconds, cfg.num_send_intervals,
               cfg.num_drain_intervals,
               fluid_csv_forward_logs_enabled() ? "buffered-forward" : "buffered-commit",
               get_configured_message_size_bytes(), sizeof(fluid_msg));
    }

    tw_run();
    merge_all_log_buffers(MPI_COMM_CODES);
    tw_end();
    return 0;
}
