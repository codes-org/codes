
/*
 * Shared interval-fluid switch/terminal implementation for:
 *   model-net-fluid-flow-wan-random-traffic
 *   model-net-fluid-flow-wan-trace-traffic
 *
 * The random-traffic workload generates persistent stochastic flows. The
 * trace-traffic workload adds measured/offered per-interval volume to persistent
 * flow ids from a CSV trace. Both retain unsent bytes at the source and inject
 * interval-sized segments at delayed advertised rates.
 * Switch LPs stage arrivals, service buffered residuals first, advertise full
 * max-min per-flow rates hop by hop, and buffer only unsent arrival residuals.
 * In statistical-hybrid mode, each switch queries a no-training per-switch
 * model for the egress-data volume of each active output-link phase.
 * Terminals
 * cache learned rates for exact destinations and shared switch-path prefixes.
 *
 * Supports sequential validation and optimistic execution. Optimistic mode uses
 * event-local reverse metadata to undo terminal generation, switch arrivals,
 * queue mutations, egress byte deltas, and RNG draws.
 */

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include <ross.h>

#include "codes_config.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"
#if CODES_HAVE_ZEROMQ
#include "codes/surrogate/director-client.h"
#include "zmqmlrequester.h"
#endif

static const char* GROUP_NAME = "FLUID_FLOW_WAN_GRP";
static const char* TERMINAL_LP_NAME = "fluid-flow-wan-terminal-lp";
static const char* SWITCH_LP_NAME = "fluid-flow-wan-switch-lp";

enum fluid_workload_mode {
    FLUID_WORKLOAD_RANDOM_TRAFFIC = 0,
    FLUID_WORKLOAD_TRACE_TRAFFIC = 1,
};

#if defined(FLUID_FLOW_WAN_RANDOM_TRAFFIC) && defined(FLUID_FLOW_WAN_TRACE_TRAFFIC)
#error "select exactly one fluid-flow WAN workload mode"
#elif defined(FLUID_FLOW_WAN_TRACE_TRAFFIC)
static constexpr fluid_workload_mode configured_workload_mode = FLUID_WORKLOAD_TRACE_TRAFFIC;
static constexpr const char* configured_workload_name = "trace-traffic";
#else
static constexpr fluid_workload_mode configured_workload_mode = FLUID_WORKLOAD_RANDOM_TRAFFIC;
static constexpr const char* configured_workload_name = "random-traffic";
#endif

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
static constexpr int MAX_SOURCE_FLOWS_PER_TERMINAL = 256;
static constexpr int MAX_FLOW_ENTRIES_PER_PORT = 256;
static constexpr int MAX_RATE_CACHE_ENTRIES = 512;
static constexpr int MAX_SIMULATION_INTERVALS = 4096;
static constexpr int INTERVAL_FLAG_WORDS = (MAX_SIMULATION_INTERVALS + 63) / 64;
static constexpr double EPS = 1e-9;


template <typename T, int Capacity> struct fixed_vector {
    T data[Capacity];
    int count;

    int size(void) const { return count; }
    T* begin(void) { return data; }
    T* end(void) { return data + count; }
    const T* begin(void) const { return data; }
    const T* end(void) const { return data + count; }
    T& operator[](int index) { return data[index]; }
    const T& operator[](int index) const { return data[index]; }

    void push_back(const T& value) {
        if (count >= Capacity) {
            tw_error(TW_LOC, "fixed_vector capacity %d exceeded", Capacity);
        }
        data[count++] = value;
    }

    void pop_back(void) {
        if (count <= 0) {
            tw_error(TW_LOC, "fixed_vector pop_back on empty container");
        }
        --count;
    }

    T* erase(T* position) {
        const int index = (int)(position - data);
        if (index < 0 || index >= count) {
            tw_error(TW_LOC, "fixed_vector erase index %d out of range", index);
        }
        for (int i = index; i + 1 < count; ++i) {
            data[i] = data[i + 1];
        }
        --count;
        return data + index;
    }

    T* erase(T* first, T* last) {
        const int begin_index = (int)(first - data);
        const int end_index = (int)(last - data);
        if (begin_index < 0 || end_index < begin_index || end_index > count) {
            tw_error(TW_LOC, "fixed_vector erase range [%d,%d) out of range", begin_index,
                     end_index);
        }
        const int removed = end_index - begin_index;
        for (int i = begin_index; i + removed < count; ++i) {
            data[i] = data[i + removed];
        }
        count -= removed;
        return data + begin_index;
    }

    T* insert(T* position, const T& value) {
        const int index = (int)(position - data);
        if (index < 0 || index > count) {
            tw_error(TW_LOC, "fixed_vector insert index %d out of range", index);
        }
        if (count >= Capacity) {
            tw_error(TW_LOC, "fixed_vector capacity %d exceeded", Capacity);
        }
        for (int i = count; i > index; --i) {
            data[i] = data[i - 1];
        }
        data[index] = value;
        ++count;
        return data + index;
    }
};

struct interval_flags {
    std::uint64_t words[INTERVAL_FLAG_WORDS];
};

static bool interval_flag_is_set(const interval_flags& flags, int interval_id) {
    const int word = interval_id / 64;
    const int bit = interval_id % 64;
    return (flags.words[word] & (std::uint64_t{1} << bit)) != 0;
}

static void interval_flag_set(interval_flags* flags, int interval_id, bool value) {
    const int word = interval_id / 64;
    const int bit = interval_id % 64;
    const std::uint64_t mask = std::uint64_t{1} << bit;
    if (value) {
        flags->words[word] |= mask;
    } else {
        flags->words[word] &= ~mask;
    }
}

static constexpr double PHASE_EARLY_SWITCH_EGRESS = 0.05;
static constexpr double PHASE_TERMINAL_WORKLOAD_GENERATE = 0.10;
static constexpr double PHASE_TERMINAL_SEND = 0.15;
/*
 * Trace offers are released immediately after the interval send boundary.
 * The 1 ns separation avoids same-timestamp ordering ambiguity while making
 * the trace interval effectively identical to the configured data interval.
 */
static constexpr double PHASE_TERMINAL_TRACE_OFFER = PHASE_TERMINAL_SEND + 1.0e-9;
static constexpr double PHASE_FLOWLET_ARRIVAL = 0.20;
static constexpr double PHASE_LATE_SWITCH_EGRESS = 0.60;

struct link_info {
    int dst_switch;
    double bandwidth_mbps;
};

struct switch_info {
    std::string name;
    int terminal_count = 0;
    int terminal_start = 0;
    double terminal_bandwidth_mbps = 100000.0;
    double switch_buffer_mbit = 64000.0;
    std::vector<link_info> links;
};

struct terminal_info {
    int switch_id = -1;
    std::string name;
};

struct trace_traffic_record {
    int interval = -1;
    unsigned long long flow_id = 0;
    int source_terminal = -1;
    int destination_terminal = -1;
    double offered_mbit = 0.0;
    int creation_interval = -1;
    int final_offer = 0;
};

static std::vector<trace_traffic_record> traffic_trace_records;

struct sim_config {
    double interval_seconds = 1.0;
    int num_send_intervals = 20;
    int num_drain_intervals = 20;
    int rng_seed = 12345;
    int flow_generation_every_n_intervals = 3;
    double random_flow_min_mbit = 10000.0;
    double random_flow_max_mbit = 50000.0;
    int debug_prints = 0;
    char egress_model[32] = "pdes";
    char topology_yaml_file[1024] = "";
    char traffic_trace_file[1024] = "";
    char terminal_log_path[1024] = "";
    char switch_log_path[1024] = "";
    char flowlet_log_path[1024] = "";
    double pause_high_watermark_fraction = 0.80;
    double pause_low_watermark_fraction = 0.50;
    double backpressure_delay_ms = 1.0;
};

enum fluid_egress_model_mode {
    FLUID_EGRESS_MODEL_PDES = 0,
    FLUID_EGRESS_MODEL_STATISTICAL = 1,
};

static fluid_egress_model_mode configured_egress_model = FLUID_EGRESS_MODEL_PDES;

static sim_config cfg;
/* Immutable process-wide topology metadata built before tw_run(). */
static std::vector<switch_info> switches;
static std::vector<terminal_info> terminals;
static std::map<std::string, int> switch_name_to_id;
static std::vector<std::vector<int>> next_switch_table;
static int total_switch_lps = 0;
static int total_terminal_lps = 0;

struct configured_unit_state {
    bool saw_mega = false;
    bool saw_giga = false;

    void observe_mega(void) { saw_mega = true; }
    void observe_giga(void) { saw_giga = true; }
};

static configured_unit_state configured_units;

static bool output_uses_gbit(void) {
    return configured_units.saw_giga && !configured_units.saw_mega;
}

static double to_output_data_unit(double mbit) {
    return output_uses_gbit() ? mbit / 1000.0 : mbit;
}

static const char* output_data_field_suffix(void) {
    return output_uses_gbit() ? "gbit" : "mbit";
}

static const char* output_data_unit_symbol(void) {
    return output_uses_gbit() ? "Gb" : "Mb";
}

struct queued_flowlet {
    int valid;
    unsigned long long flowlet_id;
    int source_terminal;
    int destination_terminal;
    int creation_interval;
    int enqueue_interval;
    int age_intervals;
    int ingress_id;
    int final_segment_sent;
    double remaining_mbit;
};

struct ingress_desc {
    int is_terminal;
    int peer_index;
    double queued_mbit;
    int pause_asserted;
};

struct port_desc {
    int is_terminal;
    int target_index;
    double capacity_mbit_per_interval;
};

struct source_flow {
    unsigned long long flow_id;
    int destination_terminal;
    int creation_interval;
    double remaining_source_mbit;
    double pending_window_mbit;
    tw_stime send_start_time_ns;
    double current_send_rate_mbps;
    int rate_epoch;
    int workload_complete;
};

enum rate_cache_key_type {
    RATE_CACHE_SWITCH_PREFIX = 1,
    RATE_CACHE_DESTINATION_TERMINAL = 2,
};

struct rate_cache_entry {
    int valid;
    int key_type;
    int key_index;
    int prefix_hops;
    std::uint64_t path_hash;
    double rate_mbps;
    int rate_epoch;
};

struct switch_rate_flow {
    unsigned long long flow_id;
    int source_terminal;
    int destination_terminal;
    int ingress_id;
    int final_segment_seen;
    double downstream_rate_mbps;
    int downstream_rate_epoch;

    /*
     * Last local max-min allocation actually advertised upstream.  Rate
     * evaluations may be triggered repeatedly while a feedback wave is
     * converging; retaining the last advertised value lets us suppress
     * numerically identical feedback and prevents control-event amplification.
     */
    double last_advertised_rate_mbps;
    int last_advertised_rate_valid;
};

struct terminal_state {
    int terminal_id;
    int attached_switch;
    unsigned long long next_flowlet_seq;
    /* Fixed LP-local storage; ROSS allocates it with terminal_state. */
    fixed_vector<source_flow, MAX_SOURCE_FLOWS_PER_TERMINAL> source_flows;
    rate_cache_entry rate_cache[MAX_RATE_CACHE_ENTRIES];
    int rate_cache_entry_count;
    double generated_mbit;
    double sent_to_switch_mbit;
    double received_mbit;
    int tx_window_active;
    int tx_window_interval;
    tw_stime tx_last_update_time_ns;
    int link_paused;
    tw_stime pause_started_at_ns;
    double total_pause_time_ns;
    unsigned long long pause_updates_received;
    unsigned long long pause_frames_received;
    unsigned long long resume_frames_received;
    int generated_flowlets;
    int received_fragments;
    unsigned long long rate_updates_received;
};

struct switch_state {
    int switch_id;
    int num_ports;
    double shared_buffer_mbit;
    port_desc ports[MAX_PORTS_PER_SWITCH];
    /* Fixed LP-local storage; no event-time heap allocation is used. */
    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT> queues[MAX_PORTS_PER_SWITCH];
    /*
     * Current-interval arrivals wait here without consuming physical buffer
     * space. SWITCH_EGRESS_LATE transmits them with capacity left after the
     * early residual-queue pass and buffers only their unsent remainder.
     */
    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT> staged_arrivals[MAX_PORTS_PER_SWITCH];
    fixed_vector<switch_rate_flow, MAX_FLOW_ENTRIES_PER_PORT> rate_flows[MAX_PORTS_PER_SWITCH];

    /*
     * Rollback-safe demand-driven egress scheduling.  Early and late egress
     * events are tracked independently for every port and interval so Time
     * Warp may have multiple future intervals outstanding at once.
     */
    interval_flags scheduled_early_egress[MAX_PORTS_PER_SWITCH];
    interval_flags scheduled_late_egress[MAX_PORTS_PER_SWITCH];
    int rate_eval_pending[MAX_PORTS_PER_SWITCH];
    int next_rate_epoch[MAX_PORTS_PER_SWITCH];
    int capacity_accounting_interval[MAX_PORTS_PER_SWITCH];
    double capacity_used_mbit[MAX_PORTS_PER_SWITCH];
    int output_link_paused[MAX_PORTS_PER_SWITCH];
    tw_stime output_link_pause_started_at_ns[MAX_PORTS_PER_SWITCH];
    double output_link_total_pause_time_ns[MAX_PORTS_PER_SWITCH];
    int pause_eval_pending;

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

    double buffered_residual_mbit;
    double sent_mbit;
    double delivered_local_mbit;
    double dropped_mbit;
    int received_fragments;
    int sent_fragments;
};

enum fluid_event_type {
    TERMINAL_WORKLOAD_GENERATE = 1,
    FLOWLET_ARRIVAL = 2,
    SWITCH_EGRESS_EARLY = 3,
    ETHERNET_PAUSE_EVAL = 4,
    SWITCH_EGRESS_LATE = 5,
    ETHERNET_PAUSE_FRAME_UPDATE = 6,
    TERMINAL_SEND = 7,
    SWITCH_RATE_EVAL = 8,
    SWITCH_RATE_FEEDBACK = 9,
    TERMINAL_RATE_UPDATE = 10,
};

static const char* backpressure_event_name(int event_type) {
    switch (event_type) {
    case ETHERNET_PAUSE_EVAL:
        return "ETHERNET_PAUSE_EVAL";
    case ETHERNET_PAUSE_FRAME_UPDATE:
        return "ETHERNET_PAUSE_FRAME_UPDATE";
    case SWITCH_RATE_EVAL:
        return "SWITCH_RATE_EVAL";
    case SWITCH_RATE_FEEDBACK:
        return "SWITCH_RATE_FEEDBACK";
    case TERMINAL_RATE_UPDATE:
        return "TERMINAL_RATE_UPDATE";
    default:
        return NULL;
    }
}

struct rc_alloc_record {
    int valid;
    int source_is_staged;
    int queue_index;
    queued_flowlet before;
    double send_mbit;
    double buffered_mbit;
    double dropped_mbit;
    int residual_queue_index;
    int residual_coalesced;
    int residual_prev_final_segment_sent;
};


struct fluid_msg {
    int event_type;
    int interval_id;
    int source_terminal;
    int destination_terminal;
    int source_switch;
    int port_id;
    int creation_interval;
    unsigned long long flowlet_id;
    int final_segment_sent;
    double mbit;
    double rate_mbps;
    int rate_epoch;
    int rate_scope_key_type;
    int rate_scope_key_index;
    int pause_asserted;
    int pause_source_switch;

    /*
     * Reverse-computation metadata. These fields are written by the forward
     * event handler and consumed by the reverse handler if the event rolls back.
     */
    int rc_rng_count;
    int rc_generated;
    int rc_terminal_flow_index;
    int rc_terminal_flow_appended;
    source_flow rc_terminal_flow_before;
    int rc_rate_cache_changed;
    int rc_rate_cache_index;
    int rc_rate_cache_was_created;
    rate_cache_entry rc_rate_cache_before;
    int rc_rate_flow_created;
    int rc_rate_flow_appended;
    int rc_rate_flow_index;
    switch_rate_flow rc_rate_flow_before;
    int rc_rate_update_applied;
    int rc_no_route;
    int rc_port_id;
    int rc_queue_index;
    int rc_coalesced;
    int rc_prev_final_segment_sent;
    double rc_accepted_mbit;
    double rc_dropped_mbit;
    int rc_alloc_count;
    int rc_pause_target_port;
    int rc_ingress_id;
    int rc_pause_change_count;
    int rc_pause_ingress_id[MAX_PAUSE_INGRESS_LINKS];
    int rc_pause_prev_asserted[MAX_PAUSE_INGRESS_LINKS];
    int rc_pause_sent_asserted[MAX_PAUSE_INGRESS_LINKS];

    int rc_terminal_tx_active;
    int rc_prev_tx_window_active;
    int rc_prev_tx_window_interval;
    tw_stime rc_prev_tx_last_update_time_ns;
    int rc_prev_pause_asserted;
    tw_stime rc_prev_pause_started_at_ns;
    double rc_prev_total_pause_time_ns;

    int rc_pause_eval_event_active;
    int rc_pause_eval_request_created;

    int rc_egress_event_active;
    int rc_egress_request_created;
    int rc_egress_request_event_type;
    int rc_egress_request_interval;
    int rc_egress_request_port;
    int rc_prev_capacity_accounting_interval;
    double rc_prev_capacity_used_mbit;

    int rc_rate_eval_event_active;
    int rc_rate_eval_request_created;
    int rc_rate_eval_request_interval;
    int rc_rate_eval_request_port;
    int rc_prev_rate_epoch;

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

static void debug_backpressure_event(const char* lp_kind, int lp_id, const fluid_msg* m,
                                     tw_lp* lp) {
    if (!cfg.debug_prints) {
        return;
    }
    const char* name = backpressure_event_name(m->event_type);
    if (name == NULL) {
        return;
    }

    const double sim_time_ns = tw_now(lp);
    fprintf(stderr,
            "[fluid-flow-wan backpressure] sim_time_ns=%.3f sim_time_ms=%.6f "
            "lp=%s id=%d event=%s interval=%d flowlet_id=%llu rate_mbps=%.12f "
            "rate_epoch=%d pause_asserted=%d pause_source_switch=%d\n",
            sim_time_ns, sim_time_ns / 1.0e6, lp_kind, lp_id, name, m->interval_id,
            (unsigned long long)m->flowlet_id, m->rate_mbps, m->rate_epoch, m->pause_asserted,
            m->pause_source_switch);
    fflush(stderr);
}

static_assert(std::is_trivially_copyable<terminal_state>::value,
              "terminal_state must remain trivially copyable for ROSS LP storage");
static_assert(std::is_trivially_copyable<switch_state>::value,
              "switch_state must remain trivially copyable for ROSS LP storage");
static_assert(std::is_trivially_copyable<fluid_msg>::value,
              "fluid_msg must remain trivially copyable for ROSS events");

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

static double milliseconds_to_ns(double milliseconds) {
    return milliseconds * 1000.0 * 1000.0;
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

static double backpressure_delay_ns(void) {
    return std::max(milliseconds_to_ns(cfg.backpressure_delay_ms), g_tw_lookahead + 1.0);
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

static double parse_scaled_quantity(const std::string& raw, const char* mega_suffix,
                                    const char* giga_suffix, const char* description) {
    const std::string s = trim(strip_quotes(raw));
    errno = 0;
    char* end = NULL;
    const double value = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || errno == ERANGE || !std::isfinite(value) || value < 0.0) {
        tw_error(TW_LOC, "invalid %s value '%s'", description, raw.c_str());
    }

    while (*end != '\0' && std::isspace((unsigned char)*end)) {
        ++end;
    }
    const std::string suffix = trim(end);

    if (suffix.empty() || suffix == mega_suffix) {
        configured_units.observe_mega();
        return value;
    }
    if (suffix == giga_suffix) {
        configured_units.observe_giga();
        return value * 1000.0;
    }

    tw_error(TW_LOC, "invalid unit in %s value '%s'; expected %s or %s", description, raw.c_str(),
             mega_suffix, giga_suffix);
    return 0.0;
}

static double parse_mbps(const std::string& raw) {
    return parse_scaled_quantity(raw, "Mbps", "Gbps", "bandwidth");
}

static double parse_mbit(const std::string& raw) {
    return parse_scaled_quantity(raw, "Mb", "Gb", "data size");
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

static void add_or_update_link(int src, int dst, double bw_mbps) {
    for (link_info& link : switches[src].links) {
        if (link.dst_switch == dst) {
            link.bandwidth_mbps = bw_mbps;
            return;
        }
    }
    link_info link;
    link.dst_switch = dst;
    link.bandwidth_mbps = bw_mbps;
    switches[src].links.push_back(link);
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
                if (!value.empty()) {
                    add_or_update_link(current_switch, dst, parse_mbps(value));
                }
                continue;
            }
            if (in_connections && indent >= 10 && current_conn_dst >= 0) {
                if (key == "bandwidth") {
                    double bw = parse_mbps(value);
                    add_or_update_link(current_switch, current_conn_dst, bw);
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
    if (configured_workload_mode == FLUID_WORKLOAD_RANDOM_TRAFFIC &&
        cfg.flow_generation_every_n_intervals <= 0) {
        cfg.flow_generation_every_n_intervals = 3;
    }
    const int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    if (total_intervals > MAX_SIMULATION_INTERVALS) {
        tw_error(TW_LOC,
                 "num_send_intervals + num_drain_intervals = %d exceeds "
                 "MAX_SIMULATION_INTERVALS=%d",
                 total_intervals, MAX_SIMULATION_INTERVALS);
    }
    if (configured_workload_mode == FLUID_WORKLOAD_RANDOM_TRAFFIC) {
        if (cfg.random_flow_min_mbit < 0.0)
            tw_error(TW_LOC, "random_flow_min must be nonnegative");
        if (cfg.random_flow_max_mbit < cfg.random_flow_min_mbit)
            tw_error(TW_LOC, "random_flow_max must be greater than or equal to random_flow_min");
    }
}

static std::vector<std::string> split_trace_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    for (char ch : line) {
        if (ch == ',') {
            fields.push_back(trim(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    fields.push_back(trim(current));
    return fields;
}

static void load_traffic_trace_csv(const char* path) {
    traffic_trace_records.clear();

    std::ifstream in(path);
    if (!in.good()) {
        tw_error(TW_LOC, "could not open traffic trace CSV file: %s", path);
    }

    std::string line;
    int line_number = 0;
    bool header_seen = false;
    double volume_scale_to_mbit = 1.0;

    struct trace_flow_meta {
        int source_terminal = -1;
        int destination_terminal = -1;
        int creation_interval = -1;
        int last_interval = -1;
        int record_count = 0;
    };
    std::map<unsigned long long, trace_flow_meta> flow_meta;
    std::set<std::pair<unsigned long long, int>> seen_flow_intervals;

    while (std::getline(in, line)) {
        ++line_number;
        const std::string stripped = trim(remove_inline_comment(line));
        if (stripped.empty()) {
            continue;
        }

        const std::vector<std::string> fields = split_trace_csv_line(stripped);
        if (!header_seen) {
            if (fields.size() != 5 || fields[0] != "interval" || fields[1] != "flow_id" ||
                fields[2] != "source_terminal" || fields[3] != "destination_terminal" ||
                (fields[4] != "offered_mbit" && fields[4] != "offered_gbit")) {
                tw_error(TW_LOC,
                         "traffic trace header must be exactly: interval,flow_id,source_terminal,"
                         "destination_terminal,offered_mbit (or offered_gbit)");
            }
            if (fields[4] == "offered_gbit") {
                volume_scale_to_mbit = 1000.0;
                configured_units.observe_giga();
            } else {
                configured_units.observe_mega();
            }
            header_seen = true;
            continue;
        }

        if (fields.size() != 5) {
            tw_error(TW_LOC, "traffic trace %s line %d has %zu fields; expected 5", path,
                     line_number, fields.size());
        }

        char* end = NULL;
        errno = 0;
        long interval_long = std::strtol(fields[0].c_str(), &end, 10);
        if (errno != 0 || end == fields[0].c_str() || *end != '\0' || interval_long < 0 ||
            interval_long >= cfg.num_send_intervals) {
            tw_error(TW_LOC, "traffic trace %s line %d has invalid interval '%s'; expected [0,%d)",
                     path, line_number, fields[0].c_str(), cfg.num_send_intervals);
        }

        errno = 0;
        end = NULL;
        unsigned long long flow_id = std::strtoull(fields[1].c_str(), &end, 10);
        if (errno != 0 || end == fields[1].c_str() || *end != '\0') {
            tw_error(TW_LOC, "traffic trace %s line %d has invalid flow_id '%s'", path, line_number,
                     fields[1].c_str());
        }

        errno = 0;
        end = NULL;
        long src_long = std::strtol(fields[2].c_str(), &end, 10);
        if (errno != 0 || end == fields[2].c_str() || *end != '\0' || src_long < 0 ||
            src_long >= (long)terminals.size()) {
            tw_error(TW_LOC, "traffic trace %s line %d has invalid source_terminal '%s'", path,
                     line_number, fields[2].c_str());
        }

        errno = 0;
        end = NULL;
        long dst_long = std::strtol(fields[3].c_str(), &end, 10);
        if (errno != 0 || end == fields[3].c_str() || *end != '\0' || dst_long < 0 ||
            dst_long >= (long)terminals.size() || dst_long == src_long) {
            tw_error(TW_LOC, "traffic trace %s line %d has invalid destination_terminal '%s'", path,
                     line_number, fields[3].c_str());
        }

        errno = 0;
        end = NULL;
        double offered = std::strtod(fields[4].c_str(), &end);
        if (errno != 0 || end == fields[4].c_str() || *end != '\0' || !std::isfinite(offered) ||
            offered <= 0.0) {
            tw_error(TW_LOC, "traffic trace %s line %d has invalid offered volume '%s'", path,
                     line_number, fields[4].c_str());
        }

        const auto flow_interval_key = std::make_pair(flow_id, (int)interval_long);
        if (!seen_flow_intervals.insert(flow_interval_key).second) {
            tw_error(TW_LOC, "traffic trace %s line %d duplicates flow_id %llu in interval %ld",
                     path, line_number, flow_id, interval_long);
        }

        trace_traffic_record record;
        record.interval = (int)interval_long;
        record.flow_id = flow_id;
        record.source_terminal = (int)src_long;
        record.destination_terminal = (int)dst_long;
        record.offered_mbit = offered * volume_scale_to_mbit;
        traffic_trace_records.push_back(record);

        auto it = flow_meta.find(flow_id);
        if (it == flow_meta.end()) {
            trace_flow_meta meta;
            meta.source_terminal = record.source_terminal;
            meta.destination_terminal = record.destination_terminal;
            meta.creation_interval = record.interval;
            meta.last_interval = record.interval;
            meta.record_count = 1;
            flow_meta[flow_id] = meta;
        } else {
            trace_flow_meta& meta = it->second;
            if (meta.source_terminal != record.source_terminal ||
                meta.destination_terminal != record.destination_terminal) {
                tw_error(TW_LOC,
                         "traffic trace flow_id %llu changes source/destination across records",
                         flow_id);
            }
            meta.creation_interval = std::min(meta.creation_interval, record.interval);
            meta.last_interval = std::max(meta.last_interval, record.interval);
            meta.record_count++;
        }
    }

    if (!header_seen) {
        tw_error(TW_LOC, "traffic trace CSV file is empty or missing a header: %s", path);
    }
    if (traffic_trace_records.empty()) {
        tw_error(TW_LOC, "traffic trace CSV file contains no traffic records: %s", path);
    }

    for (const auto& item : flow_meta) {
        const trace_flow_meta& meta = item.second;
        const int expected_records = meta.last_interval - meta.creation_interval + 1;
        if (meta.record_count != expected_records) {
            tw_error(TW_LOC,
                     "traffic trace flow_id %llu must have one positive-volume row for every "
                     "interval from %d through %d",
                     item.first, meta.creation_interval, meta.last_interval);
        }
    }

    std::sort(traffic_trace_records.begin(), traffic_trace_records.end(),
              [](const trace_traffic_record& lhs, const trace_traffic_record& rhs) {
                  if (lhs.interval != rhs.interval) {
                      return lhs.interval < rhs.interval;
                  }
                  if (lhs.source_terminal != rhs.source_terminal) {
                      return lhs.source_terminal < rhs.source_terminal;
                  }
                  return lhs.flow_id < rhs.flow_id;
              });

    for (trace_traffic_record& record : traffic_trace_records) {
        const trace_flow_meta& meta = flow_meta[record.flow_id];
        record.creation_interval = meta.creation_interval;
        record.final_offer = record.interval == meta.last_interval ? 1 : 0;
    }
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
    char tmp[128];
    memset(tmp, 0, sizeof(tmp));
    if (configuration_get_value(&config, section, key, NULL, tmp, sizeof(tmp)) > 0) {
        snprintf(value, len, "%s", tmp);
    }
}

static void read_data_quantity_param(const char* section, const char* key, double* value) {
    char raw[128];
    memset(raw, 0, sizeof(raw));
    if (configuration_get_value(&config, section, key, NULL, raw, sizeof(raw)) > 0) {
        *value = parse_mbit(raw);
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
    configured_units = configured_unit_state{};

    read_relpath_param("FLUID_FLOW_WAN", "topology_yaml_file", cfg.topology_yaml_file,
                       sizeof(cfg.topology_yaml_file));
    if (configured_workload_mode == FLUID_WORKLOAD_TRACE_TRAFFIC) {
        read_relpath_param("FLUID_FLOW_WAN", "traffic_trace_file", cfg.traffic_trace_file,
                           sizeof(cfg.traffic_trace_file));
    }
    read_relpath_param("FLUID_FLOW_WAN", "terminal_log_path", cfg.terminal_log_path,
                       sizeof(cfg.terminal_log_path));
    read_relpath_param("FLUID_FLOW_WAN", "switch_log_path", cfg.switch_log_path,
                       sizeof(cfg.switch_log_path));
    read_relpath_param("FLUID_FLOW_WAN", "flowlet_log_path", cfg.flowlet_log_path,
                       sizeof(cfg.flowlet_log_path));
    read_double_param("FLUID_FLOW_WAN", "pause_high_watermark_fraction",
                      &cfg.pause_high_watermark_fraction);
    read_double_param("FLUID_FLOW_WAN", "pause_low_watermark_fraction",
                      &cfg.pause_low_watermark_fraction);
    read_double_param("FLUID_FLOW_WAN", "backpressure_delay_ms", &cfg.backpressure_delay_ms);
    read_double_param("FLUID_FLOW_WAN", "interval_seconds", &cfg.interval_seconds);
    read_int_param("FLUID_FLOW_WAN", "num_send_intervals", &cfg.num_send_intervals);
    read_int_param("FLUID_FLOW_WAN", "num_drain_intervals", &cfg.num_drain_intervals);
    read_int_param("FLUID_FLOW_WAN", "rng_seed", &cfg.rng_seed);
    if (configured_workload_mode == FLUID_WORKLOAD_RANDOM_TRAFFIC) {
        read_int_param("FLUID_FLOW_WAN", "flow_generation_every_n_intervals",
                       &cfg.flow_generation_every_n_intervals);
        read_data_quantity_param("FLUID_FLOW_WAN", "random_flow_min", &cfg.random_flow_min_mbit);
        read_data_quantity_param("FLUID_FLOW_WAN", "random_flow_max", &cfg.random_flow_max_mbit);
    }
    read_int_param("FLUID_FLOW_WAN", "debug_prints", &cfg.debug_prints);
    read_string_param("FLUID_FLOW_WAN", "egress_model", cfg.egress_model, sizeof(cfg.egress_model));

    if (strcmp(cfg.egress_model, "pdes") == 0 || strcmp(cfg.egress_model, "pure-pdes") == 0 ||
        strcmp(cfg.egress_model, "pure_pdes") == 0) {
        configured_egress_model = FLUID_EGRESS_MODEL_PDES;
        snprintf(cfg.egress_model, sizeof(cfg.egress_model), "pdes");
    } else if (strcmp(cfg.egress_model, "statistical") == 0 ||
               strcmp(cfg.egress_model, "stat") == 0) {
        configured_egress_model = FLUID_EGRESS_MODEL_STATISTICAL;
        snprintf(cfg.egress_model, sizeof(cfg.egress_model), "statistical");
    } else {
        tw_error(TW_LOC, "FLUID_FLOW_WAN.egress_model must be pdes or statistical; got '%s'",
                 cfg.egress_model);
    }

#if !CODES_HAVE_ZEROMQ
    if (configured_egress_model != FLUID_EGRESS_MODEL_PDES) {
        tw_error(TW_LOC,
                 "FLUID_FLOW_WAN.egress_model=%s requires a CODES build with ZeroMQ enabled",
                 cfg.egress_model);
    }
#endif

    if (cfg.backpressure_delay_ms <= 0.0) {
        tw_error(TW_LOC, "backpressure_delay_ms must be positive, got %.6f",
                 cfg.backpressure_delay_ms);
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
    if (configured_workload_mode == FLUID_WORKLOAD_TRACE_TRAFFIC) {
        if (cfg.traffic_trace_file[0] == '\0') {
            tw_error(TW_LOC,
                     "FLUID_FLOW_WAN.traffic_trace_file is required for trace-traffic workload");
        }
        load_traffic_trace_csv(cfg.traffic_trace_file);
    }
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

    if (port_id < 0 || port_id >= ns->num_ports) {
        if (dropped_out != NULL) {
            *dropped_out = m->mbit;
        }
        return 0.0;
    }

    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& qv = ns->queues[port_id];

    double port_queued_before = 0.0;
    for (int i = 0; i < qv.size(); ++i) {
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

    for (int i = 0; i < qv.size(); ++i) {
        if (!qv[i].valid) {
            continue;
        }

        if (qv[i].flowlet_id == m->flowlet_id && qv[i].source_terminal == m->source_terminal &&
            qv[i].destination_terminal == m->destination_terminal &&
            qv[i].creation_interval == m->creation_interval &&
            qv[i].ingress_id == m->rc_ingress_id) {
            qv[i].remaining_mbit += accepted;
            qv[i].age_intervals = m->interval_id - m->creation_interval;
            qv[i].final_segment_sent |= m->final_segment_sent;
            ns->buffered_residual_mbit += accepted;

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
    q.final_segment_sent = m->final_segment_sent;
    q.remaining_mbit = accepted;

    if (qv.size() >= MAX_FLOW_ENTRIES_PER_PORT) {
        tw_error(TW_LOC,
                 "switch %d port %d residual queue exceeded "
                 "MAX_FLOW_ENTRIES_PER_PORT=%d",
                 ns->switch_id, port_id, MAX_FLOW_ENTRIES_PER_PORT);
    }
    qv.push_back(q);
    if (queue_index_out != NULL) {
        *queue_index_out = (int)qv.size() - 1;
    }
    ns->buffered_residual_mbit += accepted;

    if (flowlet_remaining_after_out != NULL) {
        *flowlet_remaining_after_out = accepted;
    }
    if (shared_queued_after_out != NULL) {
        *shared_queued_after_out = shared_queued_before + accepted;
    }

    return accepted;
}

static double stage_flowlet_arrival(switch_state* ns, int port_id, const fluid_msg* m,
                                    int* coalesced_out, int* queue_index_out,
                                    double* staged_remaining_after_out) {
    if (coalesced_out != NULL) {
        *coalesced_out = 0;
    }
    if (queue_index_out != NULL) {
        *queue_index_out = -1;
    }
    if (staged_remaining_after_out != NULL) {
        *staged_remaining_after_out = 0.0;
    }

    if (port_id < 0 || port_id >= ns->num_ports) {
        return 0.0;
    }

    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& staged = ns->staged_arrivals[port_id];
    for (int i = 0; i < staged.size(); ++i) {
        queued_flowlet& q = staged[i];
        if (!q.valid) {
            continue;
        }
        if (q.enqueue_interval == m->interval_id && q.flowlet_id == m->flowlet_id &&
            q.source_terminal == m->source_terminal &&
            q.destination_terminal == m->destination_terminal &&
            q.creation_interval == m->creation_interval && q.ingress_id == m->rc_ingress_id) {
            q.remaining_mbit += m->mbit;
            q.age_intervals = m->interval_id - m->creation_interval;
            q.final_segment_sent |= m->final_segment_sent;
            if (coalesced_out != NULL) {
                *coalesced_out = 1;
            }
            if (queue_index_out != NULL) {
                *queue_index_out = (int)i;
            }
            if (staged_remaining_after_out != NULL) {
                *staged_remaining_after_out = q.remaining_mbit;
            }
            return m->mbit;
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
    q.final_segment_sent = m->final_segment_sent;
    q.remaining_mbit = m->mbit;
    if (staged.size() >= MAX_FLOW_ENTRIES_PER_PORT) {
        tw_error(TW_LOC,
                 "switch %d port %d staged-arrival queue exceeded "
                 "MAX_FLOW_ENTRIES_PER_PORT=%d",
                 ns->switch_id, port_id, MAX_FLOW_ENTRIES_PER_PORT);
    }
    staged.push_back(q);

    if (queue_index_out != NULL) {
        *queue_index_out = (int)staged.size() - 1;
    }
    if (staged_remaining_after_out != NULL) {
        *staged_remaining_after_out = m->mbit;
    }
    return m->mbit;
}


static double queued_mbit_on_port(const switch_state* ns, int port_id) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        return 0.0;
    }
    double total = 0.0;
    const fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& qv = ns->queues[port_id];
    for (int i = 0; i < qv.size(); ++i) {
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
    if (port_id < 0 || port_id >= ns->num_ports) {
        return 0;
    }
    int count = 0;
    const fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& qv = ns->queues[port_id];
    for (int i = 0; i < qv.size(); ++i) {
        if (qv[i].valid && qv[i].remaining_mbit > EPS) {
            ++count;
        }
    }
    return count;
}

static int find_queue_index_for_flowlet(const switch_state* ns, int port_id,
                                        const queued_flowlet& needle) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        return -1;
    }

    const fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& qv = ns->queues[port_id];

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

static int find_staged_index_for_msg(const switch_state* ns, int port_id, const fluid_msg* m) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        return -1;
    }

    const fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& staged =
        ns->staged_arrivals[port_id];
    for (int i = 0; i < (int)staged.size(); ++i) {
        if (!staged[i].valid) {
            continue;
        }
        if (staged[i].enqueue_interval == m->interval_id && staged[i].flowlet_id == m->flowlet_id &&
            staged[i].source_terminal == m->source_terminal &&
            staged[i].destination_terminal == m->destination_terminal &&
            staged[i].creation_interval == m->creation_interval &&
            staged[i].ingress_id == m->rc_ingress_id) {
            return i;
        }
    }
    return -1;
}

static void compact_staged_arrivals(switch_state* ns, int port_id) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        return;
    }
    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& staged = ns->staged_arrivals[port_id];
    staged.erase(std::remove_if(staged.begin(), staged.end(),
                                [](const queued_flowlet& q) {
                                    return !q.valid || q.remaining_mbit <= EPS;
                                }),
                 staged.end());
}

static void compact_port_queue(switch_state* ns, int port_id) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        return;
    }
    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& qv = ns->queues[port_id];
    qv.erase(std::remove_if(qv.begin(), qv.end(),
                            [](const queued_flowlet& q) {
                                return !q.valid || q.remaining_mbit <= EPS;
                            }),
             qv.end());
}

static bool fluid_commit_logging_active = false;
/* Process-owned output buffers; these are committed logs, not LP rollback state. */
static std::ostringstream terminal_log_buffer;
static std::ostringstream switch_log_buffer;
static std::ostringstream flowlet_log_buffer;

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
        << ',' << to_output_data_unit(mbit) << '\n';
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
        << target_index << ',' << to_output_data_unit(capacity_mbit) << ','
        << to_output_data_unit(queued_before_mbit) << ',' << to_output_data_unit(sent_mbit) << ','
        << to_output_data_unit(queued_after_mbit) << ','
        << to_output_data_unit(shared_queued_before_mbit) << ','
        << to_output_data_unit(shared_queued_after_mbit) << ','
        << to_output_data_unit(shared_buffer_mbit) << ',' << to_output_data_unit(dropped_mbit)
        << ',' << active_queue_entries << '\n';
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
        << (interval_id - creation_interval) << ',' << to_output_data_unit(capacity_mbit) << ','
        << to_output_data_unit(queued_before_mbit) << ',' << to_output_data_unit(send_mbit) << ','
        << to_output_data_unit(remaining_after_mbit) << ',' << to_output_data_unit(dropped_mbit)
        << '\n';
    append_log_row(cfg.flowlet_log_path, &flowlet_log_buffer, row.str());
}

static int next_terminal_generate_interval_after(int interval_id) {
    if (interval_id < 0) {
        return 0;
    }
    return interval_id + cfg.flow_generation_every_n_intervals;
}

static int first_terminal_generate_interval(void) {
    /*
     * The first eligible terminal workload interval is always 0.  Later events
     * advance by flow_generation_every_n_intervals, so terminals do not carry a
     * speculative self-event chain through intervals where no workload can be
     * generated.
     */
    return 0;
}

static void schedule_workload_generate(terminal_state* ns, int interval_id, tw_lp* lp) {
    if (interval_id >= cfg.num_send_intervals) {
        return;
    }

    /*
     * Defensive alignment: callers should already pass eligible send intervals,
     * but if a future caller passes an arbitrary interval, round up to the next
     * eligible workload-generation interval instead of scheduling a no-op event.
     */
    const int period = cfg.flow_generation_every_n_intervals;
    int rem = interval_id % period;
    if (rem != 0) {
        interval_id += period - rem;
    }

    if (interval_id >= cfg.num_send_intervals) {
        return;
    }

    tw_event* e =
        tw_event_new(lp->gid, delay_until_ns(interval_id, PHASE_TERMINAL_WORKLOAD_GENERATE, lp),
                     lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = TERMINAL_WORKLOAD_GENERATE;
    m->interval_id = interval_id;
    m->source_terminal = ns->terminal_id;
    tw_event_send(e);
}

static void schedule_trace_offers(const terminal_state* ns, tw_lp* lp) {
    int previous_interval = -1;
    int same_interval_ordinal = 0;

    for (const trace_traffic_record& record : traffic_trace_records) {
        if (record.source_terminal != ns->terminal_id) {
            continue;
        }
        if (record.interval != previous_interval) {
            previous_interval = record.interval;
            same_interval_ordinal = 0;
        }

        const double offer_phase =
            PHASE_TERMINAL_TRACE_OFFER + (double)same_interval_ordinal * 1.0e-9;
        ++same_interval_ordinal;
        if (offer_phase >= PHASE_FLOWLET_ARRIVAL) {
            tw_error(TW_LOC,
                     "terminal %d has too many trace flows in interval %d to assign unique "
                     "trace-offer timestamps",
                     ns->terminal_id, record.interval);
        }

        tw_event* e = tw_event_new(lp->gid, delay_until_ns(record.interval, offer_phase, lp), lp);
        fluid_msg* m = (fluid_msg*)tw_event_data(e);
        memset(m, 0, sizeof(*m));
        m->event_type = TERMINAL_WORKLOAD_GENERATE;
        m->interval_id = record.interval;
        m->source_terminal = record.source_terminal;
        m->destination_terminal = record.destination_terminal;
        m->creation_interval = record.creation_interval;
        m->flowlet_id = record.flow_id;
        m->mbit = record.offered_mbit;
        /* Reused on trace-offer events to indicate that no later offer exists. */
        m->final_segment_sent = record.final_offer;
        tw_event_send(e);
    }
}

static void schedule_terminal_send(int interval_id, tw_lp* lp) {
    const int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    if (interval_id < 0 || interval_id > total_intervals) {
        return;
    }
    tw_event* e = tw_event_new(lp->gid, delay_until_ns(interval_id, PHASE_TERMINAL_SEND, lp), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = TERMINAL_SEND;
    m->interval_id = interval_id;
    tw_event_send(e);
}

static void schedule_switch_rate_eval(int interval_id, int port_id, tw_lp* lp) {
    const int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    if (interval_id < 0 || interval_id >= total_intervals) {
        return;
    }
    tw_event* e = tw_event_new(lp->gid, backpressure_delay_ns(), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = SWITCH_RATE_EVAL;
    m->interval_id = interval_id;
    m->port_id = port_id;
    tw_event_send(e);
}

static void undo_requested_switch_rate_eval(switch_state* ns, fluid_msg* m) {
    if (!m->rc_rate_eval_request_created) {
        return;
    }

    const int port_id = m->rc_rate_eval_request_port;
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid rollback rate-eval request: switch %d port %d", ns->switch_id,
                 port_id);
    }
    if (!ns->rate_eval_pending[port_id]) {
        tw_error(TW_LOC, "rollback expected pending rate-eval: switch %d port %d", ns->switch_id,
                 port_id);
    }
    ns->rate_eval_pending[port_id] = 0;
}

static void request_switch_rate_eval(switch_state* ns, int interval_id, int port_id, tw_lp* lp,
                                     fluid_msg* cause_msg) {
    const int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    if (interval_id < 0 || interval_id >= total_intervals) {
        return;
    }
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid rate-eval request on switch %d port %d", ns->switch_id, port_id);
    }

    if (ns->rate_eval_pending[port_id]) {
        return;
    }
    if (cause_msg != NULL && cause_msg->rc_rate_eval_request_created) {
        tw_error(TW_LOC, "event attempted to create more than one rate-eval request on switch %d",
                 ns->switch_id);
    }

    ns->rate_eval_pending[port_id] = 1;
    schedule_switch_rate_eval(interval_id, port_id, lp);

    if (cause_msg != NULL) {
        cause_msg->rc_rate_eval_request_created = 1;
        cause_msg->rc_rate_eval_request_interval = interval_id;
        cause_msg->rc_rate_eval_request_port = port_id;
    }
}

static void schedule_terminal_rate_update(int interval_id, int terminal_id,
                                          unsigned long long flow_id, int destination_terminal,
                                          double rate_mbps, int rate_epoch, int scope_key_type,
                                          int scope_key_index, tw_lp* lp) {
    const int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    if (interval_id < 0 || interval_id >= total_intervals) {
        return;
    }
    tw_event* e = tw_event_new(get_terminal_gid(terminal_id), backpressure_delay_ns(), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = TERMINAL_RATE_UPDATE;
    m->interval_id = interval_id;
    m->source_terminal = terminal_id;
    m->destination_terminal = destination_terminal;
    m->flowlet_id = flow_id;
    m->rate_mbps = rate_mbps;
    m->rate_epoch = rate_epoch;
    m->rate_scope_key_type = scope_key_type;
    m->rate_scope_key_index = scope_key_index;
    tw_event_send(e);
}

static void schedule_switch_rate_feedback(int interval_id, int upstream_switch,
                                          int downstream_switch, unsigned long long flow_id,
                                          int source_terminal, int destination_terminal,
                                          double rate_mbps, int rate_epoch, int scope_key_type,
                                          int scope_key_index, tw_lp* lp) {
    const int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    if (interval_id < 0 || interval_id >= total_intervals) {
        return;
    }
    tw_event* e = tw_event_new(get_switch_gid(upstream_switch), backpressure_delay_ns(), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = SWITCH_RATE_FEEDBACK;
    m->interval_id = interval_id;
    m->source_switch = downstream_switch;
    m->source_terminal = source_terminal;
    m->destination_terminal = destination_terminal;
    m->flowlet_id = flow_id;
    m->rate_mbps = rate_mbps;
    m->rate_epoch = rate_epoch;
    m->rate_scope_key_type = scope_key_type;
    m->rate_scope_key_index = scope_key_index;
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

static interval_flags* scheduled_egress_flags(switch_state* ns, int event_type, int port_id) {
    if (event_type == SWITCH_EGRESS_EARLY) {
        return &ns->scheduled_early_egress[port_id];
    }
    if (event_type == SWITCH_EGRESS_LATE) {
        return &ns->scheduled_late_egress[port_id];
    }
    tw_error(TW_LOC, "invalid switch egress event type %d", event_type);
    return NULL;
}

static void undo_requested_switch_egress(switch_state* ns, fluid_msg* m) {
    if (!m->rc_egress_request_created) {
        return;
    }

    interval_flags* flags =
        scheduled_egress_flags(ns, m->rc_egress_request_event_type, m->rc_egress_request_port);
    int interval_id = m->rc_egress_request_interval;
    if (flags == NULL || interval_id < 0 || interval_id >= MAX_SIMULATION_INTERVALS) {
        tw_error(TW_LOC, "invalid rollback egress request: switch %d port %d interval %d",
                 ns->switch_id, m->rc_egress_request_port, interval_id);
    }
    if (!interval_flag_is_set(*flags, interval_id)) {
        tw_error(TW_LOC, "rollback expected scheduled egress flag: switch %d port %d interval %d",
                 ns->switch_id, m->rc_egress_request_port, interval_id);
    }
    interval_flag_set(flags, interval_id, false);
}

static void request_switch_egress(switch_state* ns, int event_type, int interval_id, int port_id,
                                  tw_lp* lp, fluid_msg* cause_msg) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid request_switch_egress port %d on switch %d", port_id,
                 ns->switch_id);
    }

    int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    if (interval_id < 0 || interval_id >= total_intervals) {
        return;
    }

    interval_flags* flags = scheduled_egress_flags(ns, event_type, port_id);
    if (flags == NULL || interval_id >= MAX_SIMULATION_INTERVALS) {
        tw_error(TW_LOC, "egress scheduling interval %d out of range on switch %d port %d",
                 interval_id, ns->switch_id, port_id);
    }
    if (interval_flag_is_set(*flags, interval_id)) {
        return;
    }

    if (cause_msg != NULL && cause_msg->rc_egress_request_created) {
        tw_error(TW_LOC, "event attempted to create more than one egress request on switch %d",
                 ns->switch_id);
    }

    interval_flag_set(flags, interval_id, true);
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
    tw_event* e = tw_event_new(dst_gid, backpressure_delay_ns(), lp);
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

static void request_ethernet_pause_eval(switch_state* ns, int interval_id, tw_lp* lp,
                                        fluid_msg* cause_msg) {
    if (ns->pause_eval_pending) {
        return;
    }
    if (cause_msg != NULL && cause_msg->rc_pause_eval_request_created) {
        tw_error(TW_LOC,
                 "event attempted to create more than one PAUSE evaluation request on "
                 "switch %d",
                 ns->switch_id);
    }

    ns->pause_eval_pending = 1;
    tw_event* e = tw_event_new(lp->gid, backpressure_delay_ns(), lp);
    fluid_msg* pause_msg = (fluid_msg*)tw_event_data(e);
    memset(pause_msg, 0, sizeof(*pause_msg));
    pause_msg->event_type = ETHERNET_PAUSE_EVAL;
    pause_msg->interval_id = interval_id;
    tw_event_send(e);

    if (cause_msg != NULL) {
        cause_msg->rc_pause_eval_request_created = 1;
    }
}

static void undo_requested_ethernet_pause_eval(switch_state* ns, fluid_msg* m) {
    if (!m->rc_pause_eval_request_created) {
        return;
    }
    if (!ns->pause_eval_pending) {
        tw_error(TW_LOC, "rollback expected pending PAUSE evaluation on switch %d", ns->switch_id);
    }
    ns->pause_eval_pending = 0;
}

static void record_pause_change(fluid_msg* m, int ingress_id, const ingress_desc& ingress,
                                int sent_asserted) {
    if (m->rc_pause_change_count >= MAX_PAUSE_INGRESS_LINKS) {
        tw_error(TW_LOC, "too many PAUSE ingress changes in one event");
    }
    int i = m->rc_pause_change_count++;
    m->rc_pause_ingress_id[i] = ingress_id;
    m->rc_pause_prev_asserted[i] = ingress.pause_asserted;
    m->rc_pause_sent_asserted[i] = sent_asserted;
}

static void handle_ethernet_pause_eval(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    m->rc_pause_eval_event_active = 0;
    m->rc_pause_change_count = 0;

    if (!ns->pause_eval_pending) {
        return;
    }
    ns->pause_eval_pending = 0;
    m->rc_pause_eval_event_active = 1;

    const double shared_occupancy_mbit = queued_mbit_on_switch(ns);

    /*
     * The physical buffer is shared. PAUSE decisions therefore use global
     * high/low watermarks, while per-ingress queued_mbit is used only to choose
     * which directly connected senders should be throttled.
     *
     * PAUSE is represented as persistent asserted/resumed link state in this
     * interval-fluid model. This avoids a millisecond polling/refresh event
     * stream while still applying each state transition after the configured
     * backpressure propagation delay.
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
            send_pause_update_for_ingress(ns, ingress_id, m->interval_id, 0, lp);
        }
    } else if (shared_occupancy_mbit + EPS >= ns->pause_high_watermark_mbit) {
        double covered_mbit = 0.0;
        bool has_paused_ingress = false;
        int candidates[MAX_PAUSE_INGRESS_LINKS];
        int candidate_count = 0;

        for (int ingress_id = 0; ingress_id < ns->num_ingress_links; ++ingress_id) {
            const ingress_desc& ingress = ns->ingress_links[ingress_id];
            if (ingress.pause_asserted) {
                has_paused_ingress = true;
                covered_mbit += ingress.queued_mbit;
            } else if (ingress.queued_mbit > EPS) {
                candidates[candidate_count++] = ingress_id;
            }
        }

        std::sort(candidates, candidates + candidate_count, [ns](int lhs, int rhs) {
            const double lhs_mbit = ns->ingress_links[lhs].queued_mbit;
            const double rhs_mbit = ns->ingress_links[rhs].queued_mbit;
            if (std::fabs(lhs_mbit - rhs_mbit) > EPS) {
                return lhs_mbit > rhs_mbit;
            }
            return lhs < rhs;
        });

        const double excess_mbit = shared_occupancy_mbit - ns->pause_high_watermark_mbit;
        for (int candidate = 0; candidate < candidate_count; ++candidate) {
            const int ingress_id = candidates[candidate];
            if (has_paused_ingress && covered_mbit + EPS >= excess_mbit) {
                break;
            }

            ingress_desc& ingress = ns->ingress_links[ingress_id];
            record_pause_change(m, ingress_id, ingress, 1);
            ingress.pause_asserted = 1;
            has_paused_ingress = true;
            covered_mbit += ingress.queued_mbit;
            send_pause_update_for_ingress(ns, ingress_id, m->interval_id, 1, lp);
        }
    }
}

static void schedule_arrival(int interval_id, tw_lpid dst_gid, const fluid_msg* src_msg,
                             double mbit, tw_lp* lp) {
    tw_event* e = tw_event_new(dst_gid, delay_until_ns(interval_id, PHASE_FLOWLET_ARRIVAL, lp), lp);
    fluid_msg* m = (fluid_msg*)tw_event_data(e);
    memset(m, 0, sizeof(*m));
    m->event_type = FLOWLET_ARRIVAL;
    m->interval_id = interval_id;
    m->source_terminal = src_msg->source_terminal;
    m->destination_terminal = src_msg->destination_terminal;
    m->source_switch = src_msg->source_switch;
    m->creation_interval = src_msg->creation_interval;
    m->flowlet_id = src_msg->flowlet_id;
    m->final_segment_sent = src_msg->final_segment_sent;
    m->mbit = mbit;
    tw_event_send(e);
}


static int find_source_flow_index(const terminal_state* ns, unsigned long long flow_id) {
    for (int i = 0; i < (int)ns->source_flows.size(); ++i) {
        const source_flow& f = ns->source_flows[i];
        if (f.flow_id == flow_id) {
            return i;
        }
    }
    return -1;
}

static std::uint64_t extend_path_hash(std::uint64_t hash, int switch_id) {
    /* FNV-1a over fixed-width switch ids. */
    const std::uint32_t value = (std::uint32_t)switch_id;
    for (int byte = 0; byte < 4; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffu;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int find_reusable_source_flow_index(const terminal_state* ns) {
    for (int i = 0; i < ns->source_flows.size(); ++i) {
        if (ns->source_flows[i].workload_complete &&
            ns->source_flows[i].remaining_source_mbit <= EPS &&
            ns->source_flows[i].pending_window_mbit <= EPS) {
            return i;
        }
    }
    return -1;
}

static int find_rate_cache_index(const terminal_state* ns, int key_type, int key_index,
                                 int prefix_hops, std::uint64_t path_hash) {
    for (int i = 0; i < MAX_RATE_CACHE_ENTRIES; ++i) {
        const rate_cache_entry& entry = ns->rate_cache[i];
        if (!entry.valid || entry.key_type != key_type || entry.key_index != key_index) {
            continue;
        }
        if (key_type == RATE_CACHE_DESTINATION_TERMINAL ||
            (entry.prefix_hops == prefix_hops && entry.path_hash == path_hash)) {
            return i;
        }
    }
    return -1;
}

static int find_free_rate_cache_index(const terminal_state* ns) {
    for (int i = 0; i < MAX_RATE_CACHE_ENTRIES; ++i) {
        if (!ns->rate_cache[i].valid) {
            return i;
        }
    }
    return -1;
}

static bool route_prefix_identity(int source_switch, int destination_switch, int prefix_end_switch,
                                  int* prefix_hops_out, std::uint64_t* path_hash_out) {
    int current_switch = source_switch;
    int hop_count = 0;
    std::uint64_t path_hash = UINT64_C(1469598103934665603);
    path_hash = extend_path_hash(path_hash, source_switch);

    while (current_switch != destination_switch) {
        const int next_switch = next_switch_table[current_switch][destination_switch];
        if (next_switch < 0 || next_switch == current_switch) {
            return false;
        }
        ++hop_count;
        if (hop_count > (int)switches.size()) {
            tw_error(TW_LOC, "route loop while constructing rate-cache key");
        }
        path_hash = extend_path_hash(path_hash, next_switch);
        if (next_switch == prefix_end_switch) {
            *prefix_hops_out = hop_count;
            *path_hash_out = path_hash;
            return true;
        }
        current_switch = next_switch;
    }
    return false;
}

static double cached_initial_rate_mbps(const terminal_state* ns, int destination_terminal) {
    const double access_rate = switches[ns->attached_switch].terminal_bandwidth_mbps;
    double cached_rate = access_rate;

    const int exact_index =
        find_rate_cache_index(ns, RATE_CACHE_DESTINATION_TERMINAL, destination_terminal, 0, 0);
    if (exact_index >= 0) {
        cached_rate = std::min(cached_rate, ns->rate_cache[exact_index].rate_mbps);
    }

    const int destination_switch = terminals[destination_terminal].switch_id;
    int current_switch = ns->attached_switch;
    int hop_count = 0;
    std::uint64_t path_hash = UINT64_C(1469598103934665603);
    path_hash = extend_path_hash(path_hash, current_switch);

    /* Apply every known constraint on the selected route prefix. */
    while (current_switch != destination_switch) {
        const int next_switch = next_switch_table[current_switch][destination_switch];
        if (next_switch < 0 || next_switch == current_switch) {
            break;
        }
        ++hop_count;
        if (hop_count > (int)switches.size()) {
            tw_error(TW_LOC, "route loop while looking up terminal %d rate cache", ns->terminal_id);
        }
        path_hash = extend_path_hash(path_hash, next_switch);

        const int cache_index =
            find_rate_cache_index(ns, RATE_CACHE_SWITCH_PREFIX, next_switch, hop_count, path_hash);
        if (cache_index >= 0) {
            cached_rate = std::min(cached_rate, ns->rate_cache[cache_index].rate_mbps);
        }
        current_switch = next_switch;
    }

    return std::max(0.0, cached_rate);
}

static bool update_rate_cache(terminal_state* ns, fluid_msg* m, double new_rate_mbps) {
    m->rc_rate_cache_changed = 0;
    m->rc_rate_cache_index = -1;
    m->rc_rate_cache_was_created = 0;

    if (m->rate_scope_key_type != RATE_CACHE_SWITCH_PREFIX &&
        m->rate_scope_key_type != RATE_CACHE_DESTINATION_TERMINAL) {
        return false;
    }

    int prefix_hops = 0;
    std::uint64_t path_hash = 0;
    if (m->rate_scope_key_type == RATE_CACHE_SWITCH_PREFIX) {
        const int destination_switch = terminals[m->destination_terminal].switch_id;
        if (!route_prefix_identity(ns->attached_switch, destination_switch, m->rate_scope_key_index,
                                   &prefix_hops, &path_hash)) {
            return false;
        }
    }

    int index = find_rate_cache_index(ns, m->rate_scope_key_type, m->rate_scope_key_index,
                                      prefix_hops, path_hash);
    if (index < 0) {
        index = find_free_rate_cache_index(ns);
        if (index < 0) {
            tw_error(TW_LOC, "terminal %d exhausted MAX_RATE_CACHE_ENTRIES=%d", ns->terminal_id,
                     MAX_RATE_CACHE_ENTRIES);
        }
        rate_cache_entry& entry = ns->rate_cache[index];
        memset(&entry, 0, sizeof(entry));
        entry.valid = 1;
        entry.key_type = m->rate_scope_key_type;
        entry.key_index = m->rate_scope_key_index;
        entry.prefix_hops = prefix_hops;
        entry.path_hash = path_hash;
        entry.rate_mbps = new_rate_mbps;
        entry.rate_epoch = m->rate_epoch;
        ns->rate_cache_entry_count++;
        m->rc_rate_cache_changed = 1;
        m->rc_rate_cache_index = index;
        m->rc_rate_cache_was_created = 1;
        return true;
    }

    rate_cache_entry& entry = ns->rate_cache[index];
    if (m->rate_epoch < entry.rate_epoch) {
        return false;
    }

    const double updated_rate = m->rate_epoch == entry.rate_epoch
                                    ? std::min(entry.rate_mbps, new_rate_mbps)
                                    : new_rate_mbps;
    if (m->rate_epoch == entry.rate_epoch && std::fabs(updated_rate - entry.rate_mbps) <= EPS) {
        return false;
    }

    m->rc_rate_cache_before = entry;
    m->rc_rate_cache_changed = 1;
    m->rc_rate_cache_index = index;
    entry.rate_mbps = updated_rate;
    entry.rate_epoch = m->rate_epoch;
    return true;
}

static void terminal_init(terminal_state* ns, tw_lp* lp) {
    memset(ns, 0, sizeof(*ns));
    ns->terminal_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    if (ns->terminal_id < 0 || ns->terminal_id >= (int)terminals.size()) {
        tw_error(TW_LOC, "terminal LP relative id %d out of range", ns->terminal_id);
    }
    ns->attached_switch = terminals[ns->terminal_id].switch_id;
    ns->next_flowlet_seq = 0;
    ns->tx_window_active = 0;
    ns->tx_window_interval = -1;
    ns->tx_last_update_time_ns = 0.0;
    ns->link_paused = 0;
    ns->pause_started_at_ns = 0.0;
    ns->total_pause_time_ns = 0.0;
    if (configured_workload_mode == FLUID_WORKLOAD_RANDOM_TRAFFIC) {
        schedule_workload_generate(ns, first_terminal_generate_interval(), lp);
    } else {
        schedule_trace_offers(ns, lp);
    }
    schedule_terminal_send(0, lp);
}

static int choose_random_destination(int self, tw_lp* lp) {
    int n = (int)terminals.size();
    if (n <= 1) {
        return self;
    }
    int offset = (int)tw_rand_integer(lp->rng, 1, n - 1);
    return (self + offset) % n;
}

static double random_flow_size_mbit(tw_lp* lp) {
    double u = tw_rand_unif(lp->rng);
    return cfg.random_flow_min_mbit + u * (cfg.random_flow_max_mbit - cfg.random_flow_min_mbit);
}

static void compute_max_min_allocations(const double* requested, int count, double budget,
                                        double* allocations) {
    for (int i = 0; i < count; ++i) {
        allocations[i] = 0.0;
    }
    double remaining = std::max(0.0, budget);
    while (remaining > EPS) {
        int unsatisfied = 0;
        for (int i = 0; i < count; ++i) {
            if (requested[i] - allocations[i] > EPS) {
                ++unsatisfied;
            }
        }
        if (unsatisfied == 0) {
            break;
        }
        const double share = remaining / unsatisfied;
        double allocated_round = 0.0;
        for (int i = 0; i < count; ++i) {
            const double need = requested[i] - allocations[i];
            if (need <= EPS) {
                continue;
            }
            const double give = std::min(share, need);
            allocations[i] += give;
            allocated_round += give;
        }
        if (allocated_round <= EPS) {
            break;
        }
        remaining -= allocated_round;
        if (remaining < 0.0 && remaining > -EPS) {
            remaining = 0.0;
        }
    }
}

static void log_terminal_generate_event(const terminal_state* ns, const fluid_msg* m) {
    if (m->rc_generated) {
        append_terminal_log(m->interval_id,
                            configured_workload_mode == FLUID_WORKLOAD_TRACE_TRAFFIC
                                ? "trace_offer"
                                : "generate_flow",
                            ns->terminal_id, m->destination_terminal, m->mbit);
    }
}

static void log_terminal_send_event(const terminal_state* ns, const fluid_msg* m) {
    const int send_interval =
        m->rc_prev_tx_window_active ? m->rc_prev_tx_window_interval : m->interval_id;
    for (int i = 0; i < m->rc_alloc_count; ++i) {
        const rc_alloc_record& rc = m->rc_allocs[i];
        if (rc.valid && rc.send_mbit > EPS) {
            append_terminal_log(send_interval, "send", ns->terminal_id,
                                rc.before.destination_terminal, rc.send_mbit);
        }
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
        append_flowlet_log(m->interval_id,
                           m->rc_coalesced ? "stage_arrival_coalesce" : "stage_arrival",
                           ns->switch_id, m->rc_port_id, m->rc_log_target_is_terminal,
                           m->rc_log_target_index, m->flowlet_id, m->source_terminal,
                           m->destination_terminal, m->creation_interval, m->rc_log_capacity_mbit,
                           m->rc_log_port_queued_before_mbit, 0.0,
                           m->rc_log_flowlet_remaining_after_mbit, 0.0);
    }
}

static void log_switch_egress_event(const switch_state* ns, const fluid_msg* m) {
    for (int i = 0; i < m->rc_alloc_count; ++i) {
        const rc_alloc_record* rc = &m->rc_allocs[i];

        if (!rc->valid) {
            continue;
        }

        double remaining_after = rc->before.remaining_mbit - rc->send_mbit;
        if (remaining_after < 0.0 && remaining_after > -EPS) {
            remaining_after = 0.0;
        }

        if (rc->send_mbit > EPS) {
            append_flowlet_log(m->interval_id,
                               rc->source_is_staged ? "allocate_send_arrival"
                                                    : "allocate_send_buffered",
                               ns->switch_id, m->port_id, m->rc_log_target_is_terminal,
                               m->rc_log_target_index, rc->before.flowlet_id,
                               rc->before.source_terminal, rc->before.destination_terminal,
                               rc->before.creation_interval, m->rc_log_capacity_mbit,
                               m->rc_log_port_queued_before_mbit, rc->send_mbit, remaining_after,
                               0.0);
        }
        if (rc->buffered_mbit > EPS) {
            append_flowlet_log(m->interval_id, "enqueue_residual", ns->switch_id, m->port_id,
                               m->rc_log_target_is_terminal, m->rc_log_target_index,
                               rc->before.flowlet_id, rc->before.source_terminal,
                               rc->before.destination_terminal, rc->before.creation_interval,
                               m->rc_log_capacity_mbit, m->rc_log_port_queued_before_mbit, 0.0,
                               rc->buffered_mbit, 0.0);
        }
        if (rc->dropped_mbit > EPS) {
            append_flowlet_log(m->interval_id, "drop_shared_buffer_overflow", ns->switch_id,
                               m->port_id, m->rc_log_target_is_terminal, m->rc_log_target_index,
                               rc->before.flowlet_id, rc->before.source_terminal,
                               rc->before.destination_terminal, rc->before.creation_interval,
                               m->rc_log_capacity_mbit, m->rc_log_port_queued_before_mbit, 0.0, 0.0,
                               rc->dropped_mbit);
        }
    }

    append_switch_log(m->interval_id, "egress", ns->switch_id, m->port_id,
                      m->rc_log_target_is_terminal, m->rc_log_target_index, m->rc_log_capacity_mbit,
                      m->rc_log_port_queued_before_mbit, m->rc_log_sent_total_mbit,
                      m->rc_log_port_queued_after_mbit, m->rc_log_shared_queued_before_mbit,
                      m->rc_log_shared_queued_after_mbit, ns->shared_buffer_mbit,
                      m->rc_dropped_mbit, m->rc_log_active_after_entries);
}

static void handle_random_workload_generate(terminal_state* ns, fluid_msg* m, tw_lp* lp) {
    const int interval = m->interval_id;
    m->rc_rng_count = 0;
    m->rc_generated = 0;
    m->rc_terminal_flow_index = -1;
    m->rc_terminal_flow_appended = 0;

    const int dst = choose_random_destination(ns->terminal_id, lp);
    m->rc_rng_count++;
    const double total_mbit = random_flow_size_mbit(lp);
    m->rc_rng_count++;

    if (total_mbit > EPS) {
        int flow_index = find_reusable_source_flow_index(ns);
        if (flow_index < 0 && ns->source_flows.size() >= MAX_SOURCE_FLOWS_PER_TERMINAL) {
            tw_error(TW_LOC,
                     "terminal %d has %d concurrent source flows, exceeding "
                     "MAX_SOURCE_FLOWS_PER_TERMINAL=%d",
                     ns->terminal_id, ns->source_flows.size(), MAX_SOURCE_FLOWS_PER_TERMINAL);
        }
        source_flow flow;
        memset(&flow, 0, sizeof(flow));
        flow.flow_id = ((unsigned long long)ns->terminal_id << 48) |
                       (unsigned long long)ns->next_flowlet_seq++;
        flow.destination_terminal = dst;
        flow.creation_interval = interval;
        flow.remaining_source_mbit = total_mbit;
        flow.pending_window_mbit = 0.0;
        flow.send_start_time_ns = event_time_ns(interval, PHASE_TERMINAL_SEND);
        flow.current_send_rate_mbps = cached_initial_rate_mbps(ns, dst);
        flow.rate_epoch = -1;
        flow.workload_complete = 1;

        if (flow_index >= 0) {
            m->rc_terminal_flow_before = ns->source_flows[flow_index];
            ns->source_flows[flow_index] = flow;
        } else {
            ns->source_flows.push_back(flow);
            flow_index = ns->source_flows.size() - 1;
            m->rc_terminal_flow_appended = 1;
        }
        ns->generated_mbit += total_mbit;
        ns->generated_flowlets++;

        m->rc_generated = 1;
        m->rc_terminal_flow_index = flow_index;
        m->destination_terminal = dst;
        m->flowlet_id = flow.flow_id;
        m->mbit = total_mbit;
        log_terminal_generate_event(ns, m);
    }

    schedule_workload_generate(ns, next_terminal_generate_interval_after(interval), lp);
}

static void handle_trace_workload_generate(terminal_state* ns, fluid_msg* m, tw_lp* lp) {
    (void)lp;
    m->rc_rng_count = 0;
    m->rc_generated = 0;
    m->rc_terminal_flow_index = -1;
    m->rc_terminal_flow_appended = 0;

    int flow_index = find_source_flow_index(ns, m->flowlet_id);
    if (flow_index < 0) {
        if (ns->source_flows.size() >= MAX_SOURCE_FLOWS_PER_TERMINAL) {
            tw_error(TW_LOC,
                     "terminal %d has %d trace flows, exceeding "
                     "MAX_SOURCE_FLOWS_PER_TERMINAL=%d",
                     ns->terminal_id, ns->source_flows.size(), MAX_SOURCE_FLOWS_PER_TERMINAL);
        }

        source_flow flow;
        memset(&flow, 0, sizeof(flow));
        flow.flow_id = m->flowlet_id;
        flow.destination_terminal = m->destination_terminal;
        flow.creation_interval = m->creation_interval;
        flow.remaining_source_mbit = 0.0;
        flow.pending_window_mbit = 0.0;
        flow.send_start_time_ns = tw_now(lp);
        flow.current_send_rate_mbps = cached_initial_rate_mbps(ns, m->destination_terminal);
        flow.rate_epoch = -1;
        flow.workload_complete = 0;
        ns->source_flows.push_back(flow);
        flow_index = ns->source_flows.size() - 1;
        m->rc_terminal_flow_appended = 1;
        ns->generated_flowlets++;
    } else {
        source_flow& existing = ns->source_flows[flow_index];
        if (existing.destination_terminal != m->destination_terminal) {
            tw_error(TW_LOC, "trace flow %llu changed destination at terminal %d", m->flowlet_id,
                     ns->terminal_id);
        }
        m->rc_terminal_flow_before = existing;
        if (existing.remaining_source_mbit <= EPS && existing.pending_window_mbit <= EPS) {
            existing.send_start_time_ns = tw_now(lp);
        }
    }

    source_flow& flow = ns->source_flows[flow_index];
    flow.remaining_source_mbit += m->mbit;
    if (m->final_segment_sent) {
        flow.workload_complete = 1;
    }

    ns->generated_mbit += m->mbit;
    m->rc_generated = 1;
    m->rc_terminal_flow_index = flow_index;
    log_terminal_generate_event(ns, m);
}

static void handle_workload_generate(terminal_state* ns, fluid_msg* m, tw_lp* lp) {
    if (configured_workload_mode == FLUID_WORKLOAD_TRACE_TRAFFIC) {
        handle_trace_workload_generate(ns, m, lp);
    } else {
        handle_random_workload_generate(ns, m, lp);
    }
}

static void prepare_terminal_tx_rc(terminal_state* ns, fluid_msg* m) {
    if (m->rc_terminal_tx_active) {
        return;
    }
    m->rc_terminal_tx_active = 1;
    m->rc_alloc_count = 0;
    m->rc_prev_tx_window_active = ns->tx_window_active;
    m->rc_prev_tx_window_interval = ns->tx_window_interval;
    m->rc_prev_tx_last_update_time_ns = ns->tx_last_update_time_ns;
}

static rc_alloc_record* record_terminal_flow_before(terminal_state* ns, fluid_msg* m,
                                                    int flow_index) {
    for (int i = 0; i < m->rc_alloc_count; ++i) {
        if (m->rc_allocs[i].valid && m->rc_allocs[i].queue_index == flow_index) {
            return &m->rc_allocs[i];
        }
    }
    if (m->rc_alloc_count >= MAX_RC_ALLOCATIONS) {
        tw_error(TW_LOC, "terminal %d exceeded MAX_RC_ALLOCATIONS=%d", ns->terminal_id,
                 MAX_RC_ALLOCATIONS);
    }
    source_flow& flow = ns->source_flows[flow_index];
    rc_alloc_record* rc = &m->rc_allocs[m->rc_alloc_count++];
    memset(rc, 0, sizeof(*rc));
    rc->valid = 1;
    rc->queue_index = flow_index;
    rc->before.flowlet_id = flow.flow_id;
    rc->before.destination_terminal = flow.destination_terminal;
    rc->before.creation_interval = flow.creation_interval;
    rc->before.remaining_mbit = flow.remaining_source_mbit;
    /* Terminal events reuse buffered_mbit as the pre-event pending-window value. */
    rc->buffered_mbit = flow.pending_window_mbit;
    return rc;
}

static void advance_terminal_transmission(terminal_state* ns, tw_stime now_ns, fluid_msg* m) {
    prepare_terminal_tx_rc(ns, m);
    if (!ns->tx_window_active || now_ns <= ns->tx_last_update_time_ns + EPS) {
        ns->tx_last_update_time_ns = now_ns;
        return;
    }

    const tw_stime active_start_ns = ns->tx_last_update_time_ns;
    const double active_seconds =
        ns->link_paused ? 0.0 : std::max(0.0, now_ns - active_start_ns) / 1.0e9;
    if (active_seconds <= 0.0) {
        ns->tx_last_update_time_ns = now_ns;
        return;
    }

    double requested[MAX_SOURCE_FLOWS_PER_TERMINAL] = {0.0};
    for (int i = 0; i < ns->source_flows.size(); ++i) {
        const source_flow& flow = ns->source_flows[i];
        if (flow.remaining_source_mbit <= EPS) {
            continue;
        }
        const tw_stime flow_start_ns = std::max(active_start_ns, flow.send_start_time_ns);
        const double flow_seconds = std::max(0.0, now_ns - flow_start_ns) / 1.0e9;
        requested[i] =
            std::min(flow.remaining_source_mbit, flow.current_send_rate_mbps * flow_seconds);
    }

    const double terminal_budget =
        switches[ns->attached_switch].terminal_bandwidth_mbps * active_seconds;
    double allocations[MAX_SOURCE_FLOWS_PER_TERMINAL] = {0.0};
    compute_max_min_allocations(requested, ns->source_flows.size(), terminal_budget, allocations);

    for (int i = 0; i < ns->source_flows.size(); ++i) {
        const double send_mbit = allocations[i];
        if (send_mbit <= EPS) {
            continue;
        }
        source_flow& flow = ns->source_flows[i];
        rc_alloc_record* rc = record_terminal_flow_before(ns, m, i);
        /* Terminal events reuse dropped_mbit as bytes integrated by this event. */
        rc->dropped_mbit += send_mbit;
        flow.remaining_source_mbit -= send_mbit;
        if (flow.remaining_source_mbit < 0.0 && flow.remaining_source_mbit > -EPS) {
            flow.remaining_source_mbit = 0.0;
        }
        flow.pending_window_mbit += send_mbit;
        ns->sent_to_switch_mbit += send_mbit;
    }
    ns->tx_last_update_time_ns = now_ns;
}

static void rollback_terminal_transmission(terminal_state* ns, fluid_msg* m) {
    if (!m->rc_terminal_tx_active) {
        return;
    }
    for (int i = m->rc_alloc_count - 1; i >= 0; --i) {
        const rc_alloc_record& rc = m->rc_allocs[i];
        if (!rc.valid) {
            continue;
        }
        source_flow& flow = ns->source_flows[rc.queue_index];
        flow.remaining_source_mbit = rc.before.remaining_mbit;
        flow.pending_window_mbit = rc.buffered_mbit;
        ns->sent_to_switch_mbit -= rc.dropped_mbit;
    }
    ns->tx_window_active = m->rc_prev_tx_window_active;
    ns->tx_window_interval = m->rc_prev_tx_window_interval;
    ns->tx_last_update_time_ns = m->rc_prev_tx_last_update_time_ns;

    /*
     * A Time Warp event may execute, roll back, and execute again. Clear the
     * event-local snapshot marker after reverse computation so the next
     * forward execution records fresh terminal state and allocation deltas.
     */
    m->rc_terminal_tx_active = 0;
    m->rc_alloc_count = 0;
}

static void handle_terminal_send(terminal_state* ns, fluid_msg* m, tw_lp* lp) {
    m->rc_pause_target_port = -1;
    prepare_terminal_tx_rc(ns, m);
    advance_terminal_transmission(ns, tw_now(lp), m);

    if (ns->tx_window_active) {
        int active_count = 0;
        for (int i = 0; i < ns->source_flows.size(); ++i) {
            source_flow& flow = ns->source_flows[i];
            if (flow.remaining_source_mbit > EPS || flow.pending_window_mbit > EPS) {
                ++active_count;
            }
            if (flow.pending_window_mbit <= EPS) {
                continue;
            }

            rc_alloc_record* rc = record_terminal_flow_before(ns, m, i);
            const double send_mbit = flow.pending_window_mbit;
            rc->send_mbit = send_mbit;

            fluid_msg out_msg;
            memset(&out_msg, 0, sizeof(out_msg));
            out_msg.event_type = FLOWLET_ARRIVAL;
            out_msg.interval_id = m->interval_id;
            out_msg.source_terminal = ns->terminal_id;
            out_msg.destination_terminal = flow.destination_terminal;
            out_msg.source_switch = ns->attached_switch;
            out_msg.creation_interval = flow.creation_interval;
            out_msg.flowlet_id = flow.flow_id;
            out_msg.final_segment_sent =
                flow.workload_complete && flow.remaining_source_mbit <= EPS;
            out_msg.mbit = send_mbit;

            schedule_arrival(m->interval_id, get_switch_gid(ns->attached_switch), &out_msg,
                             send_mbit, lp);
            flow.pending_window_mbit = 0.0;
        }
        /* PAUSE duration is accumulated at PAUSE/RESUME transition timestamps. */
    }

    const int total_intervals = cfg.num_send_intervals + cfg.num_drain_intervals;
    if (m->interval_id < total_intervals) {
        ns->tx_window_active = 1;
        ns->tx_window_interval = m->interval_id;
        ns->tx_last_update_time_ns = tw_now(lp);
        schedule_terminal_send(m->interval_id + 1, lp);
    } else {
        ns->tx_window_active = 0;
        ns->tx_window_interval = -1;
    }

    log_terminal_send_event(ns, m);
}

static void handle_terminal_rate_update(terminal_state* ns, fluid_msg* m, tw_lp* lp) {
    prepare_terminal_tx_rc(ns, m);
    advance_terminal_transmission(ns, tw_now(lp), m);
    m->rc_rate_update_applied = 0;
    m->rc_terminal_flow_index = -1;

    const double access_rate = switches[ns->attached_switch].terminal_bandwidth_mbps;
    const double new_rate = std::max(0.0, std::min(m->rate_mbps, access_rate));
    const bool cache_changed = update_rate_cache(ns, m, new_rate);

    m->rc_terminal_flow_index = find_source_flow_index(ns, m->flowlet_id);
    if (m->rc_terminal_flow_index >= 0) {
        source_flow& flow = ns->source_flows[m->rc_terminal_flow_index];
        if ((flow.remaining_source_mbit > EPS || !flow.workload_complete) &&
            m->rate_epoch >= flow.rate_epoch) {
            m->rc_terminal_flow_before = flow;
            if (m->rate_epoch == flow.rate_epoch) {
                flow.current_send_rate_mbps = std::min(flow.current_send_rate_mbps, new_rate);
            } else {
                flow.current_send_rate_mbps = new_rate;
                flow.rate_epoch = m->rate_epoch;
            }
            m->rc_rate_update_applied = 1;
        }
    }

    if (cache_changed || m->rc_rate_update_applied) {
        ns->rate_updates_received++;
    }
}

static void handle_terminal_arrival(terminal_state* ns, fluid_msg* m) {
    ns->received_mbit += m->mbit;
    ns->received_fragments++;
    log_terminal_receive_event(ns, m);
}

static void terminal_event(terminal_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;
    debug_backpressure_event("terminal", ns->terminal_id, m, lp);
    switch (m->event_type) {
    case TERMINAL_WORKLOAD_GENERATE:
        handle_workload_generate(ns, m, lp);
        break;
    case TERMINAL_SEND:
        handle_terminal_send(ns, m, lp);
        break;
    case TERMINAL_RATE_UPDATE:
        handle_terminal_rate_update(ns, m, lp);
        break;
    case FLOWLET_ARRIVAL:
        handle_terminal_arrival(ns, m);
        break;
    case ETHERNET_PAUSE_FRAME_UPDATE: {
        prepare_terminal_tx_rc(ns, m);
        advance_terminal_transmission(ns, tw_now(lp), m);
        m->rc_prev_pause_asserted = ns->link_paused;
        m->rc_prev_pause_started_at_ns = ns->pause_started_at_ns;
        m->rc_prev_total_pause_time_ns = ns->total_pause_time_ns;

        const int new_paused = m->pause_asserted ? 1 : 0;
        if (!ns->link_paused && new_paused) {
            ns->pause_started_at_ns = tw_now(lp);
        } else if (ns->link_paused && !new_paused) {
            ns->total_pause_time_ns +=
                std::max((tw_stime)0.0, tw_now(lp) - ns->pause_started_at_ns);
            ns->pause_started_at_ns = 0.0;
        }
        ns->link_paused = new_paused;
        ns->pause_updates_received++;
        if (m->pause_asserted) {
            ns->pause_frames_received++;
        } else {
            ns->resume_frames_received++;
        }
        break;
    }
    default:
        tw_error(TW_LOC, "terminal received unknown event type %d", m->event_type);
    }
}

static void terminal_rev_event(terminal_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;
    switch (m->event_type) {
    case TERMINAL_WORKLOAD_GENERATE:
        if (m->rc_generated) {
            if (m->rc_terminal_flow_appended) {
                if (m->rc_terminal_flow_index != ns->source_flows.size() - 1) {
                    tw_error(TW_LOC, "terminal flow rollback order mismatch");
                }
                ns->source_flows.pop_back();
            } else {
                ns->source_flows[m->rc_terminal_flow_index] = m->rc_terminal_flow_before;
            }
            ns->generated_mbit -= m->mbit;

            if (configured_workload_mode == FLUID_WORKLOAD_TRACE_TRAFFIC) {
                if (m->rc_terminal_flow_appended) {
                    ns->generated_flowlets--;
                }
            } else {
                ns->generated_flowlets--;
                if (ns->next_flowlet_seq == 0) {
                    tw_error(TW_LOC, "terminal %d flow sequence underflow", ns->terminal_id);
                }
                ns->next_flowlet_seq--;
            }
        }
        for (int i = 0; i < m->rc_rng_count; ++i) {
            tw_rand_reverse_unif(lp->rng);
        }
        break;
    case TERMINAL_SEND:
        rollback_terminal_transmission(ns, m);
        break;
    case TERMINAL_RATE_UPDATE:
        if (m->rc_rate_update_applied) {
            ns->source_flows[m->rc_terminal_flow_index] = m->rc_terminal_flow_before;
        }
        if (m->rc_rate_cache_changed) {
            if (m->rc_rate_cache_was_created) {
                ns->rate_cache[m->rc_rate_cache_index].valid = 0;
                if (ns->rate_cache_entry_count <= 0) {
                    tw_error(TW_LOC, "terminal %d rate-cache count underflow", ns->terminal_id);
                }
                ns->rate_cache_entry_count--;
            } else {
                ns->rate_cache[m->rc_rate_cache_index] = m->rc_rate_cache_before;
            }
        }
        if (m->rc_rate_update_applied || m->rc_rate_cache_changed) {
            ns->rate_updates_received--;
        }
        rollback_terminal_transmission(ns, m);
        break;
    case FLOWLET_ARRIVAL:
        ns->received_mbit -= m->mbit;
        ns->received_fragments--;
        break;
    case ETHERNET_PAUSE_FRAME_UPDATE:
        ns->link_paused = m->rc_prev_pause_asserted;
        ns->pause_started_at_ns = m->rc_prev_pause_started_at_ns;
        ns->total_pause_time_ns = m->rc_prev_total_pause_time_ns;
        ns->pause_updates_received--;
        if (m->pause_asserted) {
            ns->pause_frames_received--;
        } else {
            ns->resume_frames_received--;
        }
        rollback_terminal_transmission(ns, m);
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
    case TERMINAL_WORKLOAD_GENERATE:
        log_terminal_generate_event(ns, m);
        break;
    case TERMINAL_SEND:
        log_terminal_send_event(ns, m);
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
    double source_backlog_mbit = 0.0;
    int active_source_flows = 0;
    for (const source_flow& f : ns->source_flows) {
        source_backlog_mbit += f.remaining_source_mbit;
        if (f.remaining_source_mbit > EPS) {
            ++active_source_flows;
        }
    }
    const char* unit = output_data_field_suffix();
    double total_pause_time_ns = ns->total_pause_time_ns;
    if (ns->link_paused) {
        total_pause_time_ns += std::max((tw_stime)0.0, tw_now(lp) - ns->pause_started_at_ns);
    }
    const double total_pause_time_ms = total_pause_time_ns / 1.0e6;
    printf("fluid-flow-wan-terminal gid=%llu terminal=%d switch=%d generated_%s=%.6f "
           "sent_%s=%.6f source_backlog_%s=%.6f active_source_flows=%d "
           "received_%s=%.6f rate_updates_received=%llu rate_cache_entries=%d "
           "pause_updates_received=%llu "
           "pause_frames_received=%llu resume_frames_received=%llu "
           "total_pause_time_ms=%.6f generated_flows=%d received_fragments=%d\n",
           (unsigned long long)lp->gid, ns->terminal_id, ns->attached_switch, unit,
           to_output_data_unit(ns->generated_mbit), unit,
           to_output_data_unit(ns->sent_to_switch_mbit), unit,
           to_output_data_unit(source_backlog_mbit), active_source_flows, unit,
           to_output_data_unit(ns->received_mbit), ns->rate_updates_received,
           ns->rate_cache_entry_count, ns->pause_updates_received, ns->pause_frames_received,
           ns->resume_frames_received, total_pause_time_ms, ns->generated_flowlets,
           ns->received_fragments);
}

static int find_rate_flow_index(const switch_state* ns, int port_id, unsigned long long flow_id) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        return -1;
    }
    const fixed_vector<switch_rate_flow, MAX_FLOW_ENTRIES_PER_PORT>& flows =
        ns->rate_flows[port_id];
    for (int i = 0; i < (int)flows.size(); ++i) {
        if (flows[i].flow_id == flow_id) {
            return i;
        }
    }
    return -1;
}

static bool port_has_buffered_flow(const switch_state* ns, int port_id,
                                   unsigned long long flow_id) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        return false;
    }
    for (const queued_flowlet& q : ns->queues[port_id]) {
        if (q.valid && q.flowlet_id == flow_id && q.remaining_mbit > EPS) {
            return true;
        }
    }
    for (const queued_flowlet& q : ns->staged_arrivals[port_id]) {
        if (q.valid && q.flowlet_id == flow_id && q.remaining_mbit > EPS) {
            return true;
        }
    }
    return false;
}

static bool rate_flow_is_active(const switch_state* ns, int port_id, const switch_rate_flow& flow) {
    return !flow.final_segment_seen || port_has_buffered_flow(ns, port_id, flow.flow_id);
}

static int active_rate_flow_count_on_port(const switch_state* ns, int port_id) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        return 0;
    }

    int active = 0;
    for (const switch_rate_flow& flow : ns->rate_flows[port_id]) {
        if (rate_flow_is_active(ns, port_id, flow)) {
            ++active;
        }
    }
    return active;
}

static bool rate_mbps_materially_changed(double before_mbps, double after_mbps) {
    const bool before_finite = std::isfinite(before_mbps);
    const bool after_finite = std::isfinite(after_mbps);
    if (before_finite != after_finite) {
        return true;
    }
    if (!before_finite) {
        return false;
    }

    /*
     * Ignore only floating-point noise.  At a 100 Gbps rate this relative
     * tolerance is 1e-4 Mbps (100 bit/s), far below the fluid model's useful
     * resolution while still terminating identical feedback waves.
     */
    const double scale = std::max(1.0, std::max(std::fabs(before_mbps), std::fabs(after_mbps)));
    const double tolerance = std::max(EPS, 1.0e-9 * scale);
    return std::fabs(after_mbps - before_mbps) > tolerance;
}

static double rate_demand_cap_mbps(const switch_rate_flow& flow) {
    double demand = switches[terminals[flow.source_terminal].switch_id].terminal_bandwidth_mbps;
    if (std::isfinite(flow.downstream_rate_mbps)) {
        demand = std::min(demand, flow.downstream_rate_mbps);
    }
    return std::max(0.0, demand);
}

static void compute_port_rate_allocations(const switch_state* ns, int port_id,
                                          const switch_rate_flow* const* flows, int count,
                                          double* allocations) {
    if (count <= 0) {
        return;
    }
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid rate-allocation port %d on switch %d", port_id, ns->switch_id);
    }

    double requested[MAX_FLOW_ENTRIES_PER_PORT];
    for (int i = 0; i < count; ++i) {
        requested[i] = rate_demand_cap_mbps(*flows[i]);
    }

    const double capacity_mbps =
        ns->ports[port_id].capacity_mbit_per_interval / cfg.interval_seconds;
    compute_max_min_allocations(requested, count, capacity_mbps, allocations);
}

static double staged_mbit_on_port(const switch_state* ns, int port_id) {
    if (port_id < 0 || port_id >= ns->num_ports) {
        return 0.0;
    }
    double total = 0.0;
    for (const queued_flowlet& q : ns->staged_arrivals[port_id]) {
        if (q.valid && q.remaining_mbit > EPS) {
            total += q.remaining_mbit;
        }
    }
    return total;
}

static const char* statistical_egress_phase_name(int event_type) {
    if (event_type == SWITCH_EGRESS_EARLY) {
        return "early";
    }
    if (event_type == SWITCH_EGRESS_LATE) {
        return "late";
    }
    tw_error(TW_LOC, "invalid statistical egress event type %d", event_type);
    return "invalid";
}

static double query_statistical_phase_egress_mbit(const switch_state* ns, int port_id,
                                                  int interval_id, int event_type,
                                                  double remaining_capacity_mbit,
                                                  double eligible_data_mbit, int output_paused) {
    const port_desc& port = ns->ports[port_id];
    const double interval_capacity_mbit = port.capacity_mbit_per_interval;
    const double expected_mbit =
        output_paused ? 0.0 : std::min(remaining_capacity_mbit, eligible_data_mbit);

#if !CODES_HAVE_ZEROMQ
    (void)ns;
    (void)port_id;
    (void)interval_id;
    (void)event_type;
    (void)remaining_capacity_mbit;
    (void)eligible_data_mbit;
    (void)output_paused;
    (void)interval_capacity_mbit;
    (void)expected_mbit;
    tw_error(TW_LOC, "fluid-flow-wan statistical egress model requires ZeroMQ support");
    return 0.0;
#else
    const double buffered_mbit = queued_mbit_on_port(ns, port_id);
    const double staged_mbit = staged_mbit_on_port(ns, port_id);
    const char* phase = statistical_egress_phase_name(event_type);

    std::ostringstream payload;
    payload.precision(std::numeric_limits<double>::max_digits10);
    payload << "schema_version,interval,egress_phase,switch,port,target_is_terminal,"
            << "target_index,interval_seconds,interval_capacity_mbit,capacity_used_mbit,"
            << "remaining_capacity_mbit,eligible_data_mbit,buffered_mbit,staged_mbit,"
            << "shared_queued_mbit,shared_buffer_mbit,output_paused\n";
    payload << "fluid-flow-wan-egress-v1" << ',' << interval_id << ',' << phase << ','
            << ns->switch_id << ',' << port_id << ',' << port.is_terminal << ','
            << port.target_index << ',' << cfg.interval_seconds << ',' << interval_capacity_mbit
            << ',' << ns->capacity_used_mbit[port_id] << ',' << remaining_capacity_mbit << ','
            << eligible_data_mbit << ',' << buffered_mbit << ',' << staged_mbit << ','
            << queued_mbit_on_switch(ns) << ',' << ns->shared_buffer_mbit << ',' << output_paused
            << '\n';

    const std::vector<std::string> args = {"1", std::to_string(ns->switch_id)};
    std::vector<std::string> reply;
    const double request_start_sec = MPI_Wtime();
    try {
        reply = zmqml_director_request("fluid-flow-wan", "statistical", "inference", args,
                                       payload.str());
    } catch (const std::exception& exc) {
        tw_error(TW_LOC,
                 "fluid-flow-wan statistical egress inference failed on switch %d port %d "
                 "phase %s: %s",
                 ns->switch_id, port_id, phase, exc.what());
    } catch (...) {
        tw_error(TW_LOC,
                 "fluid-flow-wan statistical egress inference failed on switch %d port %d "
                 "phase %s",
                 ns->switch_id, port_id, phase);
    }

    const double request_total_sec = MPI_Wtime() - request_start_sec;

    double server_processing_sec = 0.0;
    if (reply.size() > 1) {
        char* endptr = NULL;
        const double parsed = strtod(reply[1].c_str(), &endptr);
        if (endptr != reply[1].c_str() && std::isfinite(parsed) && parsed >= 0.0) {
            server_processing_sec = parsed;
        } else if (cfg.debug_prints) {
            fprintf(stderr,
                    "[fluid-flow-wan zmq latency] warning: could not parse server processing "
                    "time switch=%d port=%d interval=%d phase=%s ret[1]=%s\n",
                    ns->switch_id, port_id, interval_id, phase, reply[1].c_str());
            fflush(stderr);
        }
    } else if (cfg.debug_prints) {
        fprintf(stderr,
                "[fluid-flow-wan zmq latency] warning: reply has no server processing time "
                "switch=%d port=%d interval=%d phase=%s reply_fields=%llu\n",
                ns->switch_id, port_id, interval_id, phase, (unsigned long long)reply.size());
        fflush(stderr);
    }

    director_record_external_zmq_latency(server_processing_sec, request_total_sec);

    const double client_transport_sec = std::max(0.0, request_total_sec - server_processing_sec);
    if (cfg.debug_prints) {
        fprintf(stderr,
                "[fluid-flow-wan zmq latency] switch=%d port=%d interval=%d phase=%s "
                "processing_sec=%.9f total_sec=%.9f client_transport_sec=%.9f\n",
                ns->switch_id, port_id, interval_id, phase, server_processing_sec,
                request_total_sec, client_transport_sec);
        fflush(stderr);
    }

    if (reply.size() < 3 || reply[0] != "done") {
        tw_error(TW_LOC,
                 "fluid-flow-wan statistical egress inference returned an invalid reply on "
                 "switch %d port %d phase %s",
                 ns->switch_id, port_id, phase);
    }

    std::istringstream values(reply[2]);
    double prediction = 0.0;
    double extra = 0.0;
    if (!(values >> prediction) || !std::isfinite(prediction) || (values >> extra)) {
        tw_error(TW_LOC,
                 "fluid-flow-wan statistical egress inference returned malformed prediction "
                 "on switch %d port %d phase %s",
                 ns->switch_id, port_id, phase);
    }

    const double tolerance =
        1.0e-9 * std::max(1.0, std::max(interval_capacity_mbit, eligible_data_mbit));
    if (prediction < -tolerance || prediction > expected_mbit + tolerance) {
        tw_error(TW_LOC,
                 "fluid-flow-wan statistical egress prediction %.17g Mbit is outside "
                 "[0, %.17g] on switch %d port %d interval %d phase %s",
                 prediction, expected_mbit, ns->switch_id, port_id, interval_id, phase);
    }
    if (std::fabs(prediction - expected_mbit) > tolerance) {
        tw_error(TW_LOC,
                 "fluid-flow-wan statistical egress mismatch on switch %d port %d interval %d "
                 "phase %s: predicted %.17g Mbit expected %.17g Mbit",
                 ns->switch_id, port_id, interval_id, phase, prediction, expected_mbit);
    }

    if (cfg.debug_prints) {
        fprintf(stderr,
                "[fluid-flow-wan statistical egress] switch=%d port=%d interval=%d "
                "phase=%s remaining_capacity_mbit=%.12f eligible_data_mbit=%.12f "
                "predicted_egress_mbit=%.12f\n",
                ns->switch_id, port_id, interval_id, phase, remaining_capacity_mbit,
                eligible_data_mbit, prediction);
        fflush(stderr);
    }
    return prediction;
#endif
}

static void observe_rate_flow(switch_state* ns, int port_id, const fluid_msg* m,
                              fluid_msg* rc_msg) {
    rc_msg->rc_rate_flow_created = 0;
    rc_msg->rc_rate_flow_appended = 0;
    rc_msg->rc_rate_flow_index = find_rate_flow_index(ns, port_id, m->flowlet_id);
    fixed_vector<switch_rate_flow, MAX_FLOW_ENTRIES_PER_PORT>& flows = ns->rate_flows[port_id];
    if (rc_msg->rc_rate_flow_index >= 0) {
        switch_rate_flow& flow = flows[rc_msg->rc_rate_flow_index];
        rc_msg->rc_rate_flow_before = flow;
        flow.ingress_id = m->rc_ingress_id;
        flow.final_segment_seen |= m->final_segment_sent;
        return;
    }

    switch_rate_flow new_flow;
    memset(&new_flow, 0, sizeof(new_flow));
    new_flow.flow_id = m->flowlet_id;
    new_flow.source_terminal = m->source_terminal;
    new_flow.destination_terminal = m->destination_terminal;
    new_flow.ingress_id = m->rc_ingress_id;
    new_flow.final_segment_seen = m->final_segment_sent;
    new_flow.downstream_rate_mbps = std::numeric_limits<double>::infinity();
    new_flow.downstream_rate_epoch = -1;
    new_flow.last_advertised_rate_mbps = 0.0;
    new_flow.last_advertised_rate_valid = 0;

    if (flows.size() < MAX_FLOW_ENTRIES_PER_PORT) {
        /* Keep completed entries while space remains so late downstream
         * feedback can still traverse the reverse path and populate caches. */
        flows.push_back(new_flow);
        rc_msg->rc_rate_flow_index = flows.size() - 1;
        rc_msg->rc_rate_flow_appended = 1;
    } else {
        int reusable_index = -1;
        for (int i = 0; i < flows.size(); ++i) {
            if (!rate_flow_is_active(ns, port_id, flows[i])) {
                reusable_index = i;
                break;
            }
        }
        if (reusable_index < 0) {
            tw_error(TW_LOC,
                     "switch %d port %d has %d concurrent rate flows, "
                     "exceeding MAX_FLOW_ENTRIES_PER_PORT=%d",
                     ns->switch_id, port_id, flows.size(), MAX_FLOW_ENTRIES_PER_PORT);
        }
        rc_msg->rc_rate_flow_before = flows[reusable_index];
        flows[reusable_index] = new_flow;
        rc_msg->rc_rate_flow_index = reusable_index;
    }
    rc_msg->rc_rate_flow_created = 1;
}

static int output_port_for_destination(const switch_state* ns, int destination_terminal) {
    int dst_sw = terminals[destination_terminal].switch_id;
    if (dst_sw == ns->switch_id) {
        return find_terminal_port(ns, destination_terminal);
    }
    int next_sw = next_switch_table[ns->switch_id][dst_sw];
    return next_sw < 0 ? -1 : find_switch_link_port(ns, next_sw);
}

static void send_rate_feedback_upstream(switch_state* ns, const switch_rate_flow& flow,
                                        double rate_mbps, int rate_epoch, int scope_key_type,
                                        int scope_key_index, int interval_id, tw_lp* lp) {
    if (flow.ingress_id < 0 || flow.ingress_id >= ns->num_ingress_links) {
        tw_error(TW_LOC, "invalid rate-feedback ingress on switch %d", ns->switch_id);
    }
    const ingress_desc& ingress = ns->ingress_links[flow.ingress_id];
    if (ingress.is_terminal) {
        schedule_terminal_rate_update(interval_id, ingress.peer_index, flow.flow_id,
                                      flow.destination_terminal, rate_mbps, rate_epoch,
                                      scope_key_type, scope_key_index, lp);
    } else {
        schedule_switch_rate_feedback(interval_id, ingress.peer_index, ns->switch_id, flow.flow_id,
                                      flow.source_terminal, flow.destination_terminal, rate_mbps,
                                      rate_epoch, scope_key_type, scope_key_index, lp);
    }
}

static void handle_switch_rate_feedback(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    m->rc_rate_update_applied = 0;
    m->rc_rate_flow_created = 0;
    m->rc_rate_flow_appended = 0;
    m->rc_rate_flow_index = -1;
    m->rc_rate_eval_request_created = 0;

    const int port_id = output_port_for_destination(ns, m->destination_terminal);
    if (port_id < 0) {
        return;
    }
    const int idx = find_rate_flow_index(ns, port_id, m->flowlet_id);
    if (idx < 0) {
        return;
    }

    switch_rate_flow& flow = (ns->rate_flows[port_id])[idx];
    m->rc_rate_flow_index = idx;
    m->rc_port_id = port_id;
    m->rc_rate_flow_before = flow;

    if (m->rate_epoch < flow.downstream_rate_epoch) {
        return;
    }

    const double previous_rate_mbps = flow.downstream_rate_mbps;
    const int previous_rate_epoch = flow.downstream_rate_epoch;
    double updated_rate_mbps = std::max(0.0, m->rate_mbps);
    int updated_rate_epoch = m->rate_epoch;

    if (m->rate_epoch == flow.downstream_rate_epoch) {
        updated_rate_mbps = std::min(flow.downstream_rate_mbps, updated_rate_mbps);
        updated_rate_epoch = flow.downstream_rate_epoch;
    }

    const bool rate_changed = rate_mbps_materially_changed(previous_rate_mbps, updated_rate_mbps);
    const bool epoch_changed = updated_rate_epoch != previous_rate_epoch;
    if (!rate_changed && !epoch_changed) {
        return;
    }

    /*
     * Always consume a newer epoch, even when it carries the same numerical
     * rate, so a delayed older feedback message cannot become authoritative
     * later.  Only a material rate change needs another max-min evaluation.
     */
    flow.downstream_rate_mbps = updated_rate_mbps;
    flow.downstream_rate_epoch = updated_rate_epoch;
    m->rc_rate_update_applied = 1;

    if (rate_changed && rate_flow_is_active(ns, port_id, flow)) {
        request_switch_rate_eval(ns, m->interval_id, port_id, lp, m);
    }
}

static void handle_switch_rate_eval(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    m->rc_rate_eval_event_active = 0;
    /* rc_allocs are otherwise unused by SWITCH_RATE_EVAL; reuse them to make
     * last-advertised-rate suppression rollback-safe without increasing the
     * already-large fluid_msg footprint. */
    m->rc_alloc_count = 0;

    const int port_id = m->port_id;
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid rate-eval event on switch %d port %d interval %d", ns->switch_id,
                 port_id, m->interval_id);
    }
    if (!ns->rate_eval_pending[port_id]) {
        return;
    }

    ns->rate_eval_pending[port_id] = 0;
    m->rc_rate_eval_event_active = 1;
    m->rc_prev_rate_epoch = ns->next_rate_epoch[port_id];

    const switch_rate_flow* active_flows[MAX_FLOW_ENTRIES_PER_PORT];
    int active_flow_indices[MAX_FLOW_ENTRIES_PER_PORT];
    double allocations_mbps[MAX_FLOW_ENTRIES_PER_PORT];
    int active_count = 0;

    fixed_vector<switch_rate_flow, MAX_FLOW_ENTRIES_PER_PORT>& rate_flows = ns->rate_flows[port_id];
    for (int flow_index = 0; flow_index < rate_flows.size(); ++flow_index) {
        const switch_rate_flow& flow = rate_flows[flow_index];
        if (!rate_flow_is_active(ns, port_id, flow)) {
            continue;
        }
        if (active_count >= MAX_FLOW_ENTRIES_PER_PORT) {
            tw_error(TW_LOC, "switch %d port %d has too many active rate flows", ns->switch_id,
                     port_id);
        }
        active_flows[active_count] = &flow;
        active_flow_indices[active_count] = flow_index;
        ++active_count;
    }
    if (active_count <= 0) {
        return;
    }

    compute_port_rate_allocations(ns, port_id, active_flows, active_count, allocations_mbps);

    int changed_count = 0;
    for (int i = 0; i < active_count; ++i) {
        const switch_rate_flow& flow = rate_flows[active_flow_indices[i]];
        if (!flow.last_advertised_rate_valid ||
            rate_mbps_materially_changed(flow.last_advertised_rate_mbps, allocations_mbps[i])) {
            ++changed_count;
        }
    }
    if (changed_count <= 0) {
        return;
    }

    /* A new epoch represents an actually advertised rate wave, not merely an
     * internal reevaluation whose allocations were unchanged. */
    const int rate_epoch = ns->next_rate_epoch[port_id]++;

    const port_desc& port = ns->ports[port_id];
    const int scope_key_type =
        port.is_terminal ? RATE_CACHE_DESTINATION_TERMINAL : RATE_CACHE_SWITCH_PREFIX;
    const int scope_key_index = port.target_index;

    for (int i = 0; i < active_count; ++i) {
        const int flow_index = active_flow_indices[i];
        switch_rate_flow& flow = rate_flows[flow_index];
        if (flow.last_advertised_rate_valid &&
            !rate_mbps_materially_changed(flow.last_advertised_rate_mbps, allocations_mbps[i])) {
            continue;
        }

        if (m->rc_alloc_count >= MAX_RC_ALLOCATIONS) {
            tw_error(TW_LOC,
                     "switch %d port %d rate evaluation changed more than "
                     "MAX_RC_ALLOCATIONS=%d flows",
                     ns->switch_id, port_id, MAX_RC_ALLOCATIONS);
        }
        rc_alloc_record* rc = &m->rc_allocs[m->rc_alloc_count++];
        memset(rc, 0, sizeof(*rc));
        rc->valid = 1;
        /* SWITCH_RATE_EVAL reinterpretation of these otherwise-unused fields:
         * source_is_staged -> previous last-advertised-valid
         * queue_index      -> rate-flow index
         * send_mbit        -> previous last-advertised rate */
        rc->source_is_staged = flow.last_advertised_rate_valid;
        rc->queue_index = flow_index;
        rc->send_mbit = flow.last_advertised_rate_mbps;

        flow.last_advertised_rate_mbps = allocations_mbps[i];
        flow.last_advertised_rate_valid = 1;
        send_rate_feedback_upstream(ns, flow, allocations_mbps[i], rate_epoch, scope_key_type,
                                    scope_key_index, m->interval_id, lp);
    }
}

static void switch_init(switch_state* ns, tw_lp* lp) {
    memset(ns, 0, sizeof(*ns));
    ns->switch_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    if (ns->switch_id < 0 || ns->switch_id >= (int)switches.size()) {
        tw_error(TW_LOC, "switch LP relative id %d out of range", ns->switch_id);
    }

    for (int p = 0; p < MAX_PORTS_PER_SWITCH; ++p) {
        ns->capacity_accounting_interval[p] = -1;
        ns->capacity_used_mbit[p] = 0.0;
        ns->output_link_paused[p] = 0;
        ns->output_link_pause_started_at_ns[p] = 0.0;
        ns->output_link_total_pause_time_ns[p] = 0.0;
        ns->rate_eval_pending[p] = 0;
        ns->next_rate_epoch[p] = 0;
    }
    ns->pause_eval_pending = 0;

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
    }
    for (int upstream = 0; upstream < (int)switches.size(); ++upstream) {
        if (upstream == ns->switch_id || !switch_has_directed_link_to(upstream, ns->switch_id)) {
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
    }
    if (ns->num_ingress_links <= 0) {
        tw_error(TW_LOC, "switch %d has no ingress links", ns->switch_id);
    }
    ns->pause_high_watermark_mbit = ns->shared_buffer_mbit * cfg.pause_high_watermark_fraction;
    ns->pause_low_watermark_mbit = ns->shared_buffer_mbit * cfg.pause_low_watermark_fraction;

    for (size_t i = 0; i < sw.links.size(); ++i) {
        if (ns->num_ports >= MAX_PORTS_PER_SWITCH) {
            tw_error(TW_LOC, "too many ports on switch %d", ns->switch_id);
        }
        port_desc* p = &ns->ports[ns->num_ports++];
        memset(p, 0, sizeof(*p));
        p->is_terminal = 0;
        p->target_index = sw.links[i].dst_switch;
        p->capacity_mbit_per_interval = sw.links[i].bandwidth_mbps * cfg.interval_seconds;
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
    }

    /*
     * Egress is demand-driven.  We no longer pre-schedule periodic empty
     * egress events for every port. Arrivals schedule late egress in their
     * arrival interval. Any residual bytes admitted to the physical buffer
     * schedule early egress in the following interval.
     *
     * PAUSE hysteresis is demand-driven. Buffer occupancy changes request a
     * millisecond-scale ETHERNET_PAUSE_EVAL. Asserted PAUSE state persists
     * until a later low-watermark evaluation sends RESUME.
     *
     * Rate evaluation is demand-driven. A port is reevaluated when its active
     * flow set changes or when downstream feedback changes one flow's demand
     * cap, because full max-min allocation can change every flow on the port.
     */
}


static void handle_switch_arrival(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    m->rc_egress_request_created = 0;
    m->rc_rate_eval_request_created = 0;
    m->rc_rate_flow_created = 0;
    m->rc_rate_flow_appended = 0;
    m->rc_rate_flow_index = -1;
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
    const double port_queued_before = queued_mbit_on_port(ns, port_id);
    const double shared_queued_before = queued_mbit_on_switch(ns);
    double staged_remaining_after = 0.0;
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

    observe_rate_flow(ns, port_id, m, m);

    const int prior_staged_index = find_staged_index_for_msg(ns, port_id, m);
    m->rc_prev_final_segment_sent =
        prior_staged_index >= 0
            ? (ns->staged_arrivals[port_id])[prior_staged_index].final_segment_sent
            : 0;

    double staged_mbit =
        stage_flowlet_arrival(ns, port_id, m, &coalesced, &queue_index, &staged_remaining_after);

    m->rc_queue_index = queue_index;
    m->rc_coalesced = coalesced;
    m->rc_accepted_mbit = staged_mbit;
    m->rc_dropped_mbit = 0.0;

    m->rc_log_port_queued_before_mbit = port_queued_before;
    m->rc_log_port_queued_after_mbit = port_queued_before;
    m->rc_log_shared_queued_before_mbit = shared_queued_before;
    m->rc_log_shared_queued_after_mbit = shared_queued_before;
    m->rc_log_flowlet_remaining_after_mbit = staged_remaining_after;
    m->rc_log_active_after_entries = ns->staged_arrivals[port_id].size();

    log_switch_arrival_event(ns, m);

    if (staged_mbit > EPS) {
        request_switch_egress(ns, SWITCH_EGRESS_LATE, m->interval_id, port_id, lp, m);
    }
    if (m->rc_rate_flow_created) {
        request_switch_rate_eval(ns, m->interval_id, port_id, lp, m);
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
    out_msg.final_segment_sent = q->final_segment_sent;
    out_msg.mbit = send_mbit;

    const port_desc* p = &ns->ports[port_id];
    if (p->is_terminal) {
        tw_lpid term_gid = get_terminal_gid(p->target_index);
        schedule_arrival(interval_id + 1, term_gid, &out_msg, send_mbit, lp);
        ns->delivered_local_mbit += send_mbit;
    } else {
        tw_lpid sw_gid = get_switch_gid(p->target_index);
        schedule_arrival(interval_id + 1, sw_gid, &out_msg, send_mbit, lp);
    }
    ns->sent_mbit += send_mbit;
    ns->sent_fragments++;
}

static void handle_switch_egress(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    m->rc_egress_request_created = 0;
    m->rc_rate_eval_request_created = 0;
    m->rc_pause_eval_request_created = 0;
    m->rc_accepted_mbit = 0.0;
    m->rc_dropped_mbit = 0.0;

    int port_id = m->port_id;
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid switch egress port %d", port_id);
    }

    interval_flags* scheduled = scheduled_egress_flags(ns, m->event_type, port_id);
    m->rc_egress_event_active = scheduled != NULL && m->interval_id >= 0 &&
                                m->interval_id < MAX_SIMULATION_INTERVALS &&
                                interval_flag_is_set(*scheduled, m->interval_id);

    /* A duplicate or canceled event is a forward and reverse no-op. */
    if (!m->rc_egress_event_active) {
        return;
    }
    interval_flag_set(scheduled, m->interval_id, false);

    port_desc* p = &ns->ports[port_id];
    const int rate_active_before = active_rate_flow_count_on_port(ns, port_id);
    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& qv = ns->queues[port_id];
    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& staged = ns->staged_arrivals[port_id];
    const bool service_staged = (m->event_type == SWITCH_EGRESS_LATE);
    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& source = service_staged ? staged : qv;

    m->rc_prev_capacity_accounting_interval = ns->capacity_accounting_interval[port_id];
    m->rc_prev_capacity_used_mbit = ns->capacity_used_mbit[port_id];

    if (ns->capacity_accounting_interval[port_id] != m->interval_id) {
        ns->capacity_accounting_interval[port_id] = m->interval_id;
        ns->capacity_used_mbit[port_id] = 0.0;
    }

    const double capacity = p->capacity_mbit_per_interval;
    const double remaining_physical_capacity =
        std::max(0.0, capacity - ns->capacity_used_mbit[port_id]);
    const double eligible_phase_data_mbit =
        service_staged ? staged_mbit_on_port(ns, port_id) : queued_mbit_on_port(ns, port_id);
    const int output_paused = ns->output_link_paused[port_id];
    const double local_phase_egress_mbit =
        output_paused ? 0.0 : std::min(remaining_physical_capacity, eligible_phase_data_mbit);

    double remaining_capacity = local_phase_egress_mbit;
    if (configured_egress_model == FLUID_EGRESS_MODEL_STATISTICAL) {
        remaining_capacity =
            query_statistical_phase_egress_mbit(ns, port_id, m->interval_id, m->event_type,
                                                remaining_physical_capacity,
                                                eligible_phase_data_mbit, output_paused);
    }
    const double queued_before = queued_mbit_on_port(ns, port_id);
    const double shared_queued_before = queued_mbit_on_switch(ns);

    int active_before = 0;
    for (const queued_flowlet& q : source) {
        if (q.valid && q.remaining_mbit > EPS) {
            ++active_before;
        }
    }
    if (active_before > MAX_RC_ALLOCATIONS) {
        tw_error(TW_LOC,
                 "switch %d port %d egress has %d active flowlets, exceeding "
                 "MAX_RC_ALLOCATIONS=%d",
                 ns->switch_id, port_id, active_before, MAX_RC_ALLOCATIONS);
    }

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
    m->rc_log_active_after_entries = active_flowlet_count_on_port(ns, port_id);
    m->rc_pause_target_port = -1;

    if (output_paused) {
        remaining_capacity = 0.0;
    }

    double send_plan[MAX_FLOW_ENTRIES_PER_PORT] = {0.0};
    while (remaining_capacity > EPS) {
        int unsatisfied_count = 0;
        for (int i = 0; i < (int)source.size(); ++i) {
            if (source[i].valid && source[i].remaining_mbit - send_plan[i] > EPS) {
                ++unsatisfied_count;
            }
        }
        if (unsatisfied_count == 0) {
            break;
        }

        const double equal_share = remaining_capacity / unsatisfied_count;
        double allocated_this_round = 0.0;
        for (int i = 0; i < (int)source.size(); ++i) {
            if (!source[i].valid || source[i].remaining_mbit - send_plan[i] <= EPS) {
                continue;
            }
            const double available_for_flowlet = source[i].remaining_mbit - send_plan[i];
            const double send = std::min(equal_share, available_for_flowlet);
            send_plan[i] += send;
            allocated_this_round += send;
        }
        if (allocated_this_round <= EPS) {
            break;
        }
        remaining_capacity -= allocated_this_round;
        if (remaining_capacity < 0.0 && remaining_capacity > -EPS) {
            remaining_capacity = 0.0;
        }
    }

    double sent_total = 0.0;
    if (!service_staged) {
        /* Early egress: previously buffered residuals have strict priority. */
        for (int i = 0; i < (int)qv.size(); ++i) {
            if (!qv[i].valid || send_plan[i] <= EPS) {
                continue;
            }

            queued_flowlet before = qv[i];
            double send = std::min(send_plan[i], before.remaining_mbit);
            rc_alloc_record* rc = &m->rc_allocs[m->rc_alloc_count++];
            memset(rc, 0, sizeof(*rc));
            rc->valid = 1;
            rc->source_is_staged = 0;
            rc->queue_index = i;
            rc->before = before;
            rc->send_mbit = send;

            send_flowlet_fragment(ns, port_id, &before, send, m->interval_id, lp);
            qv[i].remaining_mbit -= send;
            ns->ingress_links[before.ingress_id].queued_mbit -= send;
            if (ns->ingress_links[before.ingress_id].queued_mbit < 0.0 &&
                ns->ingress_links[before.ingress_id].queued_mbit > -EPS) {
                ns->ingress_links[before.ingress_id].queued_mbit = 0.0;
            }
            sent_total += send;
        }
        compact_port_queue(ns, port_id);
    } else {
        /*
         * Late egress: use only capacity left by the early residual pass.
         * Bytes sent here bypass the physical buffer. Only unsent residuals
         * are admitted to the shared buffer, and overflow is dropped here.
         */
        for (int i = 0; i < (int)staged.size(); ++i) {
            if (!staged[i].valid || staged[i].remaining_mbit <= EPS) {
                continue;
            }

            queued_flowlet before = staged[i];
            double send = std::min(send_plan[i], before.remaining_mbit);
            double residual = std::max(0.0, before.remaining_mbit - send);

            rc_alloc_record* rc = &m->rc_allocs[m->rc_alloc_count++];
            memset(rc, 0, sizeof(*rc));
            rc->valid = 1;
            rc->source_is_staged = 1;
            rc->queue_index = i;
            rc->before = before;
            rc->send_mbit = send;
            rc->residual_queue_index = -1;

            if (send > EPS) {
                send_flowlet_fragment(ns, port_id, &before, send, m->interval_id, lp);
                sent_total += send;
            }

            if (residual > EPS) {
                fluid_msg residual_msg;
                memset(&residual_msg, 0, sizeof(residual_msg));
                residual_msg.interval_id = m->interval_id;
                residual_msg.source_terminal = before.source_terminal;
                residual_msg.destination_terminal = before.destination_terminal;
                residual_msg.creation_interval = before.creation_interval;
                residual_msg.flowlet_id = before.flowlet_id;
                residual_msg.final_segment_sent = before.final_segment_sent;
                residual_msg.mbit = residual;
                residual_msg.rc_ingress_id = before.ingress_id;

                const int prior_residual_index = find_queue_index_for_flowlet(ns, port_id, before);
                rc->residual_prev_final_segment_sent =
                    prior_residual_index >= 0
                        ? (ns->queues[port_id])[prior_residual_index].final_segment_sent
                        : 0;

                double ignored_port_before = 0.0;
                double ignored_shared_before = 0.0;
                double ignored_shared_after = 0.0;
                double dropped = 0.0;
                double ignored_remaining_after = 0.0;
                int coalesced = 0;
                int residual_queue_index = -1;
                double buffered =
                    enqueue_flowlet(ns, port_id, &residual_msg, &ignored_port_before,
                                    &ignored_shared_before, &ignored_shared_after, &dropped,
                                    &ignored_remaining_after, &coalesced, &residual_queue_index);

                rc->buffered_mbit = buffered;
                rc->dropped_mbit = dropped;
                rc->residual_queue_index = residual_queue_index;
                rc->residual_coalesced = coalesced;
                m->rc_accepted_mbit += buffered;
                m->rc_dropped_mbit += dropped;
                ns->ingress_links[before.ingress_id].queued_mbit += buffered;
            }

            staged[i].remaining_mbit = 0.0;
        }
        compact_staged_arrivals(ns, port_id);
    }

    ns->capacity_used_mbit[port_id] += sent_total;
    if (ns->capacity_used_mbit[port_id] > capacity + EPS) {
        tw_error(TW_LOC, "switch %d port %d exceeded interval capacity: used %.12f capacity %.12f",
                 ns->switch_id, port_id, ns->capacity_used_mbit[port_id], capacity);
    }

    const double queued_after = queued_mbit_on_port(ns, port_id);
    const double shared_queued_after = queued_mbit_on_switch(ns);
    const int active_after = active_flowlet_count_on_port(ns, port_id);
    m->rc_log_port_queued_after_mbit = queued_after;
    m->rc_log_shared_queued_after_mbit = shared_queued_after;
    m->rc_log_sent_total_mbit = sent_total;
    m->rc_log_active_after_entries = active_after;

    log_switch_egress_event(ns, m);

    if (queued_after > EPS) {
        request_switch_egress(ns, SWITCH_EGRESS_EARLY, m->interval_id + 1, port_id, lp, m);
    }

    const int rate_active_after = active_rate_flow_count_on_port(ns, port_id);
    if (rate_active_after != rate_active_before) {
        request_switch_rate_eval(ns, m->interval_id, port_id, lp, m);
    }
    if (std::fabs(shared_queued_after - shared_queued_before) > EPS) {
        request_ethernet_pause_eval(ns, m->interval_id, lp, m);
    }
}

static void handle_switch_pause_update(switch_state* ns, fluid_msg* m, tw_lp* lp) {
    int port_id = find_switch_link_port(ns, m->pause_source_switch);
    if (port_id < 0) {
        tw_error(TW_LOC,
                 "switch %d received PAUSE update from switch %d but has no output link to it",
                 ns->switch_id, m->pause_source_switch);
    }

    m->rc_pause_target_port = port_id;
    m->rc_prev_pause_asserted = ns->output_link_paused[port_id];
    m->rc_prev_pause_started_at_ns = ns->output_link_pause_started_at_ns[port_id];
    m->rc_prev_total_pause_time_ns = ns->output_link_total_pause_time_ns[port_id];

    const int new_paused = m->pause_asserted ? 1 : 0;
    if (!ns->output_link_paused[port_id] && new_paused) {
        ns->output_link_pause_started_at_ns[port_id] = tw_now(lp);
    } else if (ns->output_link_paused[port_id] && !new_paused) {
        ns->output_link_total_pause_time_ns[port_id] +=
            std::max((tw_stime)0.0, tw_now(lp) - ns->output_link_pause_started_at_ns[port_id]);
        ns->output_link_pause_started_at_ns[port_id] = 0.0;
    }
    ns->output_link_paused[port_id] = new_paused;
    ns->pause_updates_received++;
    if (m->pause_asserted) {
        ns->pause_frames_received++;
    } else {
        ns->resume_frames_received++;
    }
}

static void switch_event(switch_state* ns, tw_bf* b, fluid_msg* m, tw_lp* lp) {
    (void)b;
    debug_backpressure_event("switch", ns->switch_id, m, lp);
    switch (m->event_type) {
    case FLOWLET_ARRIVAL:
        handle_switch_arrival(ns, m, lp);
        break;
    case SWITCH_EGRESS_EARLY:
    case SWITCH_EGRESS_LATE:
        handle_switch_egress(ns, m, lp);
        break;
    case ETHERNET_PAUSE_FRAME_UPDATE:
        handle_switch_pause_update(ns, m, lp);
        break;
    case ETHERNET_PAUSE_EVAL:
        handle_ethernet_pause_eval(ns, m, lp);
        break;
    case SWITCH_RATE_FEEDBACK:
        handle_switch_rate_feedback(ns, m, lp);
        break;
    case SWITCH_RATE_EVAL:
        handle_switch_rate_eval(ns, m, lp);
        break;
    default:
        tw_error(TW_LOC, "switch received unknown event type %d", m->event_type);
    }
}

static void rollback_switch_arrival(switch_state* ns, fluid_msg* m) {
    ns->received_fragments--;

    undo_requested_switch_rate_eval(ns, m);
    undo_requested_switch_egress(ns, m);

    if (m->rc_no_route) {
        ns->dropped_mbit -= m->rc_dropped_mbit;
        return;
    }

    int port_id = m->rc_port_id;

    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid rollback arrival port %d on switch %d", port_id, ns->switch_id);
    }

    if (m->rc_accepted_mbit <= EPS) {
        return;
    }

    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& staged = ns->staged_arrivals[port_id];

    int idx = m->rc_queue_index;
    if (idx < 0 || idx >= (int)staged.size() || staged[idx].flowlet_id != m->flowlet_id) {
        idx = find_staged_index_for_msg(ns, port_id, m);
    }

    if (idx < 0) {
        tw_error(TW_LOC, "could not find flowlet %llu on switch %d port %d during arrival rollback",
                 (unsigned long long)m->flowlet_id, ns->switch_id, port_id);
    }

    if (m->rc_coalesced) {
        staged[idx].remaining_mbit -= m->rc_accepted_mbit;
        staged[idx].final_segment_sent = m->rc_prev_final_segment_sent;

        if (staged[idx].remaining_mbit <= EPS) {
            tw_error(
                TW_LOC,
                "coalesced flowlet %llu became empty during arrival rollback on switch %d port %d",
                (unsigned long long)m->flowlet_id, ns->switch_id, port_id);
        }
    } else {
        staged.erase(staged.begin() + idx);
    }

    fixed_vector<switch_rate_flow, MAX_FLOW_ENTRIES_PER_PORT>& rate_flows = ns->rate_flows[port_id];
    if (m->rc_rate_flow_created) {
        const int rate_idx = m->rc_rate_flow_index;
        if (rate_idx < 0 || rate_idx >= rate_flows.size() ||
            rate_flows[rate_idx].flow_id != m->flowlet_id) {
            tw_error(TW_LOC, "invalid created rate-flow rollback on switch %d", ns->switch_id);
        }
        if (m->rc_rate_flow_appended) {
            if (rate_idx != rate_flows.size() - 1) {
                tw_error(TW_LOC, "rate-flow rollback order mismatch on switch %d", ns->switch_id);
            }
            rate_flows.pop_back();
        } else {
            rate_flows[rate_idx] = m->rc_rate_flow_before;
        }
    } else if (m->rc_rate_flow_index >= 0) {
        rate_flows[m->rc_rate_flow_index] = m->rc_rate_flow_before;
    }
}

static void rollback_switch_egress(switch_state* ns, fluid_msg* m) {
    if (!m->rc_egress_event_active) {
        return;
    }

    int port_id = m->port_id;
    if (port_id < 0 || port_id >= ns->num_ports) {
        tw_error(TW_LOC, "invalid rollback egress port %d on switch %d", port_id, ns->switch_id);
    }

    undo_requested_ethernet_pause_eval(ns, m);
    undo_requested_switch_egress(ns, m);
    undo_requested_switch_rate_eval(ns, m);

    interval_flags* scheduled = scheduled_egress_flags(ns, m->event_type, port_id);
    if (scheduled == NULL || m->interval_id < 0 || m->interval_id >= MAX_SIMULATION_INTERVALS) {
        tw_error(TW_LOC, "invalid rollback egress interval %d on switch %d port %d", m->interval_id,
                 ns->switch_id, port_id);
    }
    interval_flag_set(scheduled, m->interval_id, true);
    ns->capacity_accounting_interval[port_id] = m->rc_prev_capacity_accounting_interval;
    ns->capacity_used_mbit[port_id] = m->rc_prev_capacity_used_mbit;
    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& qv = ns->queues[port_id];
    fixed_vector<queued_flowlet, MAX_FLOW_ENTRIES_PER_PORT>& staged = ns->staged_arrivals[port_id];
    const port_desc* p = &ns->ports[port_id];

    /*
     * Undo late-egress residual admissions in reverse insertion order. This
     * restores the physical queue to its post-early-egress state.
     */
    for (int r = m->rc_alloc_count - 1; r >= 0; --r) {
        rc_alloc_record* rc = &m->rc_allocs[r];
        if (!rc->valid || !rc->source_is_staged) {
            continue;
        }

        if (rc->buffered_mbit > EPS) {
            int idx = rc->residual_queue_index;
            if (idx < 0 || idx >= (int)qv.size() || qv[idx].flowlet_id != rc->before.flowlet_id) {
                idx = find_queue_index_for_flowlet(ns, port_id, rc->before);
            }
            if (idx < 0) {
                tw_error(TW_LOC,
                         "could not find residual flowlet %llu on switch %d port %d "
                         "during egress rollback",
                         (unsigned long long)rc->before.flowlet_id, ns->switch_id, port_id);
            }

            if (rc->residual_coalesced) {
                qv[idx].remaining_mbit -= rc->buffered_mbit;
                qv[idx].final_segment_sent = rc->residual_prev_final_segment_sent;
                if (qv[idx].remaining_mbit <= EPS) {
                    tw_error(TW_LOC,
                             "coalesced residual flowlet %llu became empty during "
                             "egress rollback on switch %d port %d",
                             (unsigned long long)rc->before.flowlet_id, ns->switch_id, port_id);
                }
            } else {
                qv.erase(qv.begin() + idx);
            }

            ns->buffered_residual_mbit -= rc->buffered_mbit;
            ns->ingress_links[rc->before.ingress_id].queued_mbit -= rc->buffered_mbit;
            if (ns->ingress_links[rc->before.ingress_id].queued_mbit < 0.0 &&
                ns->ingress_links[rc->before.ingress_id].queued_mbit > -EPS) {
                ns->ingress_links[rc->before.ingress_id].queued_mbit = 0.0;
            }
        }
        ns->dropped_mbit -= rc->dropped_mbit;

        if (rc->send_mbit > EPS) {
            ns->sent_mbit -= rc->send_mbit;
            ns->sent_fragments--;
            if (p->is_terminal) {
                ns->delivered_local_mbit -= rc->send_mbit;
            }
        }
    }

    /*
     * Restore early-egress queue entries in ascending original-index order.
     * Ascending insertion preserves the exact pre-event vector order when
     * several fully drained entries were compacted away.
     */
    for (int r = 0; r < m->rc_alloc_count; ++r) {
        rc_alloc_record* rc = &m->rc_allocs[r];
        if (!rc->valid || rc->source_is_staged || rc->send_mbit <= EPS) {
            continue;
        }

        int idx = rc->queue_index;
        if (idx < 0 || idx >= (int)qv.size() || qv[idx].flowlet_id != rc->before.flowlet_id) {
            idx = find_queue_index_for_flowlet(ns, port_id, rc->before);
        }
        if (idx >= 0) {
            qv[idx] = rc->before;
        } else {
            int insert_idx = std::max(0, std::min(rc->queue_index, (int)qv.size()));
            qv.insert(qv.begin() + insert_idx, rc->before);
        }

        ns->ingress_links[rc->before.ingress_id].queued_mbit += rc->send_mbit;
        ns->sent_mbit -= rc->send_mbit;
        ns->sent_fragments--;
        if (p->is_terminal) {
            ns->delivered_local_mbit -= rc->send_mbit;
        }
    }

    /* Late egress removes every staged entry; rebuild the original order. */
    for (int r = 0; r < m->rc_alloc_count; ++r) {
        rc_alloc_record* rc = &m->rc_allocs[r];
        if (!rc->valid || !rc->source_is_staged) {
            continue;
        }
        int insert_idx = std::max(0, std::min(rc->queue_index, (int)staged.size()));
        staged.insert(staged.begin() + insert_idx, rc->before);
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
        undo_requested_ethernet_pause_eval(ns, m);
        for (int i = m->rc_pause_change_count - 1; i >= 0; --i) {
            int ingress_id = m->rc_pause_ingress_id[i];
            ingress_desc& ingress = ns->ingress_links[ingress_id];
            ingress.pause_asserted = m->rc_pause_prev_asserted[i];
            ns->pause_updates_sent--;
            if (m->rc_pause_sent_asserted[i]) {
                ns->pause_frames_sent--;
            } else {
                ns->resume_frames_sent--;
            }
        }
        if (m->rc_pause_eval_event_active) {
            ns->pause_eval_pending = 1;
        }
        break;

    case ETHERNET_PAUSE_FRAME_UPDATE:
        ns->output_link_paused[m->rc_pause_target_port] = m->rc_prev_pause_asserted;
        ns->output_link_pause_started_at_ns[m->rc_pause_target_port] =
            m->rc_prev_pause_started_at_ns;
        ns->output_link_total_pause_time_ns[m->rc_pause_target_port] =
            m->rc_prev_total_pause_time_ns;
        ns->pause_updates_received--;
        if (m->pause_asserted) {
            ns->pause_frames_received--;
        } else {
            ns->resume_frames_received--;
        }
        break;

    case SWITCH_RATE_FEEDBACK:
        undo_requested_switch_rate_eval(ns, m);
        if (m->rc_rate_update_applied) {
            if (m->rc_port_id < 0 || m->rc_port_id >= ns->num_ports || m->rc_rate_flow_index < 0 ||
                m->rc_rate_flow_index >= (int)ns->rate_flows[m->rc_port_id].size()) {
                tw_error(TW_LOC, "invalid switch rate-feedback rollback state");
            }
            ns->rate_flows[m->rc_port_id][m->rc_rate_flow_index] = m->rc_rate_flow_before;
        }
        break;

    case SWITCH_RATE_EVAL:
        if (m->rc_rate_eval_event_active) {
            if (m->port_id < 0 || m->port_id >= ns->num_ports) {
                tw_error(TW_LOC, "invalid rate-eval rollback state");
            }
            fixed_vector<switch_rate_flow, MAX_FLOW_ENTRIES_PER_PORT>& rate_flows =
                ns->rate_flows[m->port_id];
            for (int r = m->rc_alloc_count - 1; r >= 0; --r) {
                const rc_alloc_record& rc = m->rc_allocs[r];
                if (!rc.valid || rc.queue_index < 0 || rc.queue_index >= rate_flows.size()) {
                    tw_error(TW_LOC, "invalid rate-eval advertised-rate rollback state");
                }
                switch_rate_flow& flow = rate_flows[rc.queue_index];
                flow.last_advertised_rate_valid = rc.source_is_staged;
                flow.last_advertised_rate_mbps = rc.send_mbit;
            }
            ns->rate_eval_pending[m->port_id] = 1;
            ns->next_rate_epoch[m->port_id] = m->rc_prev_rate_epoch;
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
    const char* unit = output_data_field_suffix();
    /* Sum paused duration across outputs; concurrent paused outputs count independently. */
    double total_pause_time_ns = 0.0;
    for (int p = 0; p < ns->num_ports; ++p) {
        double port_pause_time_ns = ns->output_link_total_pause_time_ns[p];
        if (ns->output_link_paused[p]) {
            port_pause_time_ns +=
                std::max((tw_stime)0.0, tw_now(lp) - ns->output_link_pause_started_at_ns[p]);
        }
        total_pause_time_ns += port_pause_time_ns;
    }
    const double total_pause_time_ms = total_pause_time_ns / 1.0e6;
    printf("fluid-flow-wan gid=%llu switch=%d name=%s ports=%d shared_buffer_%s=%.6f "
           "received_fragments=%d sent_fragments=%d buffered_residual_%s=%.6f "
           "sent_%s=%.6f local_delivery_%s=%.6f dropped_%s=%.6f ready_queue_%s=%.6f "
           "shared_buffer_occupied_%s=%.6f pause_asserted=%d pause_updates_sent=%llu "
           "pause_frames_sent=%llu resume_frames_sent=%llu pause_updates_received=%llu "
           "pause_frames_received=%llu resume_frames_received=%llu total_pause_time_ms=%.6f\n",
           (unsigned long long)lp->gid, ns->switch_id, switches[ns->switch_id].name.c_str(),
           ns->num_ports, unit, to_output_data_unit(ns->shared_buffer_mbit), ns->received_fragments,
           ns->sent_fragments, unit, to_output_data_unit(ns->buffered_residual_mbit), unit,
           to_output_data_unit(ns->sent_mbit), unit, to_output_data_unit(ns->delivered_local_mbit),
           unit, to_output_data_unit(ns->dropped_mbit), unit, to_output_data_unit(queued), unit,
           to_output_data_unit(queued), any_pause_asserted, ns->pause_updates_sent,
           ns->pause_frames_sent, ns->resume_frames_sent, ns->pause_updates_received,
           ns->pause_frames_received, ns->resume_frames_received, total_pause_time_ms);
}

const tw_optdef app_opt[] = {TWOPT_GROUP("interval-fluid switch/terminal workload"), TWOPT_END()};

static bool has_suffix(const char* value, const char* suffix) {
    const size_t value_len = strlen(value);
    const size_t suffix_len = strlen(suffix);
    return value_len >= suffix_len && strcmp(value + value_len - suffix_len, suffix) == 0;
}

static const char* find_config_arg(int argc, char** argv) {
    for (int i = argc - 1; i >= 1; --i) {
        if (argv[i] == NULL) {
            continue;
        }

        const char* arg = argv[i];
        if (has_suffix(arg, ".conf") || has_suffix(arg, ".yaml") || has_suffix(arg, ".yml") ||
            has_suffix(arg, ".json")) {
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
}

static void write_log_headers(int rank) {
    if (rank != 0) {
        return;
    }

    const char* unit = output_data_field_suffix();

    std::ostringstream terminal_header;
    terminal_header << "interval,event,terminal,terminal_name,attached_switch,peer_terminal,"
                    << unit << '\n';
    write_log_header_file(cfg.terminal_log_path, terminal_header.str().c_str());

    std::ostringstream switch_header;
    switch_header << "interval,event,switch,switch_name,port,target_type,target_index,"
                  << "capacity_" << unit << ",queued_before_" << unit << ",sent_" << unit
                  << ",queued_after_" << unit << ",shared_queued_before_" << unit
                  << ",shared_queued_after_" << unit << ",shared_buffer_" << unit << ",dropped_"
                  << unit << ",active_queue_entries\n";
    write_log_header_file(cfg.switch_log_path, switch_header.str().c_str());

    std::ostringstream flowlet_header;
    flowlet_header << "interval,event,switch,switch_name,port,target_type,target_index,"
                   << "flowlet_id,source_terminal,destination_terminal,creation_interval,"
                   << "age_intervals,capacity_" << unit << ",queued_before_" << unit << ",send_"
                   << unit << ",remaining_after_" << unit << ",dropped_" << unit << '\n';
    write_log_header_file(cfg.flowlet_log_path, flowlet_header.str().c_str());
}

static void configure_hybrid_server_debug(int rank) {
#if CODES_HAVE_ZEROMQ
    if (configured_egress_model == FLUID_EGRESS_MODEL_PDES) {
        return;
    }

    /*
     * FFW does not instantiate a Director LP, so director_init() never sends
     * the FLUID_FLOW_WAN.debug_prints setting to zmqmlserver.py. Configure the
     * shared server explicitly before any switch LP can issue an inference
     * request. Send both enabled and disabled values because the server may be
     * kept alive across multiple simulation runs.
     */
    if (rank == 0) {
        const std::vector<std::string> args = {"1", std::to_string(cfg.debug_prints != 0 ? 1 : 0)};
        std::vector<std::string> reply;

        try {
            reply = zmqml_request("set-debug", args, "");
        } catch (const std::exception& exc) {
            tw_error(TW_LOC, "failed to configure fluid-flow-wan ZeroMQ server debug mode: %s",
                     exc.what());
        } catch (...) {
            tw_error(TW_LOC, "failed to configure fluid-flow-wan ZeroMQ server debug mode");
        }

        if (reply.empty() || reply[0] != "done") {
            tw_error(TW_LOC, "ZeroMQ server rejected fluid-flow-wan debug setting");
        }
    }

    MPI_Barrier(MPI_COMM_CODES);
#else
    (void)rank;
#endif
}

int main(int argc, char** argv) {
    int rank = 0;

    g_tw_ts_end = seconds_to_ns(60.0 * 60.0 * 24.0 * 365.0);

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    const char* config_file = find_config_arg(argc, argv);
    if (config_file == NULL) {
        printf("Usage: mpirun -np <n> %s --sync=1 -- <config.yaml>\n", argv[0]);
        MPI_Finalize();
        return 1;
    }

    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    configuration_load(config_file, MPI_COMM_CODES, &config);
    load_config();
    configure_hybrid_server_debug(rank);
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
        printf("fluid-flow-wan config: workload=%s switches=%zu terminals=%zu "
               "interval_seconds=%.6f num_send_intervals=%d num_drain_intervals=%d "
               "backpressure_delay_ms=%.6f ",
               configured_workload_name, switches.size(), terminals.size(), cfg.interval_seconds,
               cfg.num_send_intervals, cfg.num_drain_intervals, cfg.backpressure_delay_ms);

        if (configured_workload_mode == FLUID_WORKLOAD_RANDOM_TRAFFIC) {
            printf("flow_generation_every_n_intervals=%d random_flow_min_%s=%.6f "
                   "random_flow_max_%s=%.6f ",
                   cfg.flow_generation_every_n_intervals, output_data_field_suffix(),
                   to_output_data_unit(cfg.random_flow_min_mbit), output_data_field_suffix(),
                   to_output_data_unit(cfg.random_flow_max_mbit));
        } else {
            printf("trace_records=%zu traffic_trace_file=%s ", traffic_trace_records.size(),
                   cfg.traffic_trace_file);
        }

        printf("output_data_unit=%s buffer_mode=shared egress_model=%s csv_logs=%s "
               "ross_message_size=%d fluid_msg_size=%zu\n",
               output_data_unit_symbol(), cfg.egress_model,
               fluid_csv_forward_logs_enabled() ? "buffered-forward" : "buffered-commit",
               get_configured_message_size_bytes(), sizeof(fluid_msg));
    }

    tw_run();
#if CODES_HAVE_ZEROMQ
    if (configured_egress_model == FLUID_EGRESS_MODEL_STATISTICAL) {
        director_print_external_zmq_latency_stats();
    }
#endif
    merge_all_log_buffers(MPI_COMM_CODES);
    tw_end();
    return 0;
}
