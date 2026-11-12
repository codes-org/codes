/*
 * Event-driven rate-based fluid WAN model.
 *
 * Core semantics:
 *   FLOW_START      source application starts a finite transfer
 *   FLOW_REGISTER   downstream path-state establishment
 *   RATE_FEEDBACK   upstream admissible-rate feedback
 *   DATA_RATE_UPDATE downstream actual-rate propagation
 *   BUFFER_THRESHOLD shared-buffer HIGH/LOW watermark crossing
 *   PAUSE_UPDATE    upstream flow-selective PAUSE/RESUME
 *   FLOW_COMPLETE   source completion propagated downstream
 *
 * Data and control both traverse one configured physical link latency.  There
 * is no fluid forwarding interval.  Data volume and buffer occupancy are
 * integrated analytically between events while rates are piecewise constant.
 */

#include <ross.h>

#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"

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
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

static const char* GROUP_NAME = "RATE_FLUID_WAN_GRP";
static const char* TERMINAL_LP_NAME = "rate-fluid-wan-terminal-lp";
static const char* SWITCH_LP_NAME = "rate-fluid-wan-switch-lp";

static constexpr int MAX_PORTS_PER_SWITCH = 128;
static constexpr int MAX_FLOWS_PER_LP = 256;
static constexpr int MAX_INGRESS_LINKS = 128;
static constexpr double EPS = 1e-9;
static constexpr double INF_RATE = 1e100;

enum rate_unit_kind { RATE_UNIT_MBPS = 0, RATE_UNIT_GBPS = 1 };
enum data_unit_kind { DATA_UNIT_MB = 0, DATA_UNIT_GB = 1 };
enum time_unit_kind { TIME_UNIT_MS = 0, TIME_UNIT_S = 1 };

struct unit_observation {
    bool saw_mbps = false;
    bool saw_gbps = false;
    bool saw_mb = false;
    bool saw_gb = false;
    bool saw_ms = false;
    bool saw_s = false;
};

static unit_observation units_seen;
static rate_unit_kind simulation_rate_unit = RATE_UNIT_MBPS;
static data_unit_kind simulation_data_unit = DATA_UNIT_MB;
static time_unit_kind output_time_unit = TIME_UNIT_MS;

struct raw_link_info {
    int dst_switch = -1;
    double bandwidth = 0.0;
};

struct raw_switch_info {
    std::string name;
    int terminal_count = 0;
    int terminal_start = 0;
    double terminal_bandwidth = 100000.0;
    double switch_buffer = 8000.0;
    std::vector<raw_link_info> links;
};

struct terminal_info {
    int switch_id = -1;
    std::string name;
};

struct flow_spec {
    unsigned long long flow_id = 0;
    int source_terminal = -1;
    int destination_terminal = -1;
    double start_seconds = 0.0;
    double size_data = 0.0;
};

struct sim_config {
    char topology_file[1024] = "";
    char flow_file[1024] = "";
    char flow_summary_file[1024] = "rate-fluid-flow-summary.csv";
    double link_latency_seconds = 0.001;
    int pause_enabled = 1;
    double pause_high_watermark_fraction = 0.80;
    double pause_low_watermark_fraction = 0.50;
    int debug = 0;
};

static sim_config cfg;
static std::vector<raw_switch_info> switches;
static std::vector<terminal_info> terminals;
static std::vector<flow_spec> flow_specs;
static std::map<std::string, int> switch_name_to_id;
static std::vector<std::vector<int>> next_switch_table;
static int total_switch_lps = 0;
static int total_terminal_lps = 0;
static int mpi_rank_global = 0;
static int mpi_size_global = 1;

static double trim_rate_from_mbps(double mbps) {
    return simulation_rate_unit == RATE_UNIT_GBPS ? mbps / 1000.0 : mbps;
}

static double trim_data_from_mb(double mb) {
    return simulation_data_unit == DATA_UNIT_GB ? mb / 1000.0 : mb;
}

static double rate_to_data_per_second(double rate) {
    if (simulation_rate_unit == RATE_UNIT_GBPS && simulation_data_unit == DATA_UNIT_GB)
        return rate / 8.0;
    if (simulation_rate_unit == RATE_UNIT_GBPS && simulation_data_unit == DATA_UNIT_MB)
        return rate * 125.0;
    if (simulation_rate_unit == RATE_UNIT_MBPS && simulation_data_unit == DATA_UNIT_GB)
        return rate / 8000.0;
    return rate / 8.0;
}

static const char* rate_unit_name() {
    return simulation_rate_unit == RATE_UNIT_GBPS ? "Gbps" : "Mbps";
}

static const char* data_unit_name() {
    return simulation_data_unit == DATA_UNIT_GB ? "GB" : "MB";
}

static const char* time_unit_name() {
    return output_time_unit == TIME_UNIT_S ? "s" : "ms";
}

static double seconds_to_output_time(double seconds) {
    return output_time_unit == TIME_UNIT_S ? seconds : seconds * 1000.0;
}

static double seconds_to_ns(double seconds) {
    return seconds * 1.0e9;
}
static double ns_to_seconds(tw_stime ns) {
    return ((double)ns) / 1.0e9;
}

static std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b]))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace((unsigned char)s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

static std::string strip_quotes(std::string s) {
    s = trim(s);
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

static std::string remove_inline_comment(const std::string& s) {
    bool in_quote = false;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((s[i] == '"' || s[i] == '\'') && (i == 0 || s[i - 1] != '\\')) {
            if (!in_quote) {
                in_quote = true;
                quote = s[i];
            } else if (quote == s[i]) {
                in_quote = false;
                quote = 0;
            }
        }
        if (!in_quote && s[i] == '#')
            return s.substr(0, i);
    }
    return s;
}

static int leading_spaces(const std::string& s) {
    int n = 0;
    while (n < (int)s.size() && s[n] == ' ')
        ++n;
    return n;
}

static bool split_key_value(const std::string& line, std::string* key, std::string* value) {
    size_t p = line.find(':');
    if (p == std::string::npos)
        return false;
    *key = trim(line.substr(0, p));
    *value = trim(line.substr(p + 1));
    return !key->empty();
}

static std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quote = false;
    for (char c : line) {
        if (c == '"') {
            quote = !quote;
            continue;
        }
        if (c == ',' && !quote) {
            out.push_back(trim(cur));
            cur.clear();
        } else
            cur.push_back(c);
    }
    out.push_back(trim(cur));
    return out;
}

static double parse_number_and_suffix(const std::string& raw, std::string* suffix) {
    std::string s = trim(strip_quotes(raw));
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (errno == ERANGE || end == s.c_str() || !std::isfinite(v) || v < 0.0)
        tw_error(TW_LOC, "invalid quantity '%s'", raw.c_str());
    while (*end && std::isspace((unsigned char)*end))
        ++end;
    *suffix = trim(end);
    return v;
}

static double parse_rate_to_mbps(const std::string& raw) {
    std::string u;
    double v = parse_number_and_suffix(raw, &u);
    if (u == "Mbps") {
        units_seen.saw_mbps = true;
        return v;
    }
    if (u == "Gbps") {
        units_seen.saw_gbps = true;
        return v * 1000.0;
    }
    tw_error(TW_LOC, "rate '%s' must use Mbps or Gbps", raw.c_str());
    return 0.0;
}

static double parse_data_to_mb(const std::string& raw, bool observe_output_unit) {
    std::string u;
    double v = parse_number_and_suffix(raw, &u);
    if (u == "MB") {
        if (observe_output_unit)
            units_seen.saw_mb = true;
        return v;
    }
    if (u == "GB") {
        if (observe_output_unit)
            units_seen.saw_gb = true;
        return v * 1000.0;
    }
    /* Legacy topology-buffer spellings are accepted and converted to bytes.
     * They do not determine the flow-summary data unit. */
    if (u == "Mb")
        return v / 8.0;
    if (u == "Gb")
        return v * 125.0;
    tw_error(TW_LOC, "data quantity '%s' must use MB or GB (legacy Mb/Gb accepted for buffers)",
             raw.c_str());
    return 0.0;
}

static double parse_time_to_seconds(const std::string& raw, bool observe_output_unit) {
    std::string u;
    double v = parse_number_and_suffix(raw, &u);
    if (u == "s" || u == "sec" || u == "second" || u == "seconds") {
        if (observe_output_unit)
            units_seen.saw_s = true;
        return v;
    }
    if (u == "ms" || u == "millisecond" || u == "milliseconds") {
        if (observe_output_unit)
            units_seen.saw_ms = true;
        return v / 1000.0;
    }
    if (u == "us" || u == "microsecond" || u == "microseconds")
        return v / 1.0e6;
    if (u == "ns" || u == "nanosecond" || u == "nanoseconds")
        return v / 1.0e9;
    tw_error(TW_LOC, "time quantity '%s' must use s, ms, us, or ns", raw.c_str());
    return 0.0;
}

static int get_or_add_switch(const std::string& name) {
    auto it = switch_name_to_id.find(name);
    if (it != switch_name_to_id.end())
        return it->second;
    int id = (int)switches.size();
    raw_switch_info sw;
    sw.name = name;
    switches.push_back(sw);
    switch_name_to_id[name] = id;
    return id;
}

static void add_or_update_link(int src, int dst, double mbps) {
    for (auto& l : switches[src].links) {
        if (l.dst_switch == dst) {
            l.bandwidth = mbps;
            return;
        }
    }
    raw_link_info l;
    l.dst_switch = dst;
    l.bandwidth = mbps;
    switches[src].links.push_back(l);
}

static void load_topology_yaml(const char* path) {
    switches.clear();
    terminals.clear();
    switch_name_to_id.clear();
    std::ifstream in(path);
    if (!in)
        tw_error(TW_LOC, "could not open topology file %s", path);
    bool in_topology = false, in_switches = false, in_connections = false;
    int current_switch = -1, current_dst = -1;
    std::string raw;
    while (std::getline(in, raw)) {
        std::string nc = remove_inline_comment(raw);
        if (trim(nc).empty())
            continue;
        int indent = leading_spaces(nc);
        std::string line = trim(nc);
        if (line == "topology:") {
            in_topology = true;
            in_switches = false;
            continue;
        }
        if (in_topology && line == "switches:") {
            in_switches = true;
            current_switch = -1;
            continue;
        }
        std::string key, value;
        if (!split_key_value(line, &key, &value) || !in_switches)
            continue;
        if (indent == 4 && value.empty()) {
            current_switch = get_or_add_switch(key);
            in_connections = false;
            current_dst = -1;
            continue;
        }
        if (current_switch < 0)
            continue;
        if (indent == 6 && key == "connections") {
            in_connections = true;
            current_dst = -1;
            continue;
        }
        if (!in_connections && indent >= 6) {
            if (key == "terminals")
                switches[current_switch].terminal_count = std::atoi(strip_quotes(value).c_str());
            else if (key == "terminal_bandwidth")
                switches[current_switch].terminal_bandwidth = parse_rate_to_mbps(value);
            else if (key == "switch_buffer")
                switches[current_switch].switch_buffer = parse_data_to_mb(value, false);
            continue;
        }
        if (in_connections && indent == 8) {
            current_dst = get_or_add_switch(key);
            if (!value.empty())
                add_or_update_link(current_switch, current_dst, parse_rate_to_mbps(value));
            continue;
        }
        if (in_connections && indent >= 10 && current_dst >= 0 && key == "bandwidth")
            add_or_update_link(current_switch, current_dst, parse_rate_to_mbps(value));
    }
    int start = 0;
    for (int s = 0; s < (int)switches.size(); ++s) {
        switches[s].terminal_start = start;
        for (int j = 0; j < switches[s].terminal_count; ++j) {
            terminal_info t;
            t.switch_id = s;
            std::ostringstream os;
            os << "T" << start;
            t.name = os.str();
            terminals.push_back(t);
            ++start;
        }
    }
    if (switches.empty())
        tw_error(TW_LOC, "topology contains no switches");
    if (terminals.size() < 2)
        tw_error(TW_LOC, "topology must contain at least two terminals");
}

static void load_flow_csv(const char* path) {
    flow_specs.clear();
    std::ifstream in(path);
    if (!in)
        tw_error(TW_LOC, "could not open flow CSV %s", path);
    std::string line;
    int lineno = 0;
    bool header = false;
    std::set<unsigned long long> ids;
    while (std::getline(in, line)) {
        ++lineno;
        std::string s = trim(remove_inline_comment(line));
        if (s.empty())
            continue;
        std::vector<std::string> f = split_csv(s);
        if (!header) {
            if (f.size() != 5 || f[0] != "start_time" || f[1] != "flow_id" ||
                f[2] != "source_terminal" || f[3] != "destination_terminal" || f[4] != "size")
                tw_error(TW_LOC, "flow CSV header must be "
                                 "start_time,flow_id,source_terminal,destination_terminal,size");
            header = true;
            continue;
        }
        if (f.size() != 5)
            tw_error(TW_LOC, "flow CSV %s line %d expected 5 fields", path, lineno);
        char* end = nullptr;
        unsigned long long id = std::strtoull(f[1].c_str(), &end, 10);
        if (end == f[1].c_str() || *end != '\0' || !ids.insert(id).second)
            tw_error(TW_LOC, "invalid or duplicate flow_id '%s' at line %d", f[1].c_str(), lineno);
        long src = std::strtol(f[2].c_str(), &end, 10);
        if (end == f[2].c_str() || *end != '\0' || src < 0 || src >= (long)terminals.size())
            tw_error(TW_LOC, "invalid source_terminal '%s' at line %d", f[2].c_str(), lineno);
        long dst = std::strtol(f[3].c_str(), &end, 10);
        if (end == f[3].c_str() || *end != '\0' || dst < 0 || dst >= (long)terminals.size() ||
            dst == src)
            tw_error(TW_LOC, "invalid destination_terminal '%s' at line %d", f[3].c_str(), lineno);
        flow_spec fs;
        fs.flow_id = id;
        fs.source_terminal = (int)src;
        fs.destination_terminal = (int)dst;
        fs.start_seconds = parse_time_to_seconds(f[0], true);
        fs.size_data = parse_data_to_mb(f[4], true);
        if (fs.size_data <= 0.0)
            tw_error(TW_LOC, "flow size must be positive at line %d", lineno);
        flow_specs.push_back(fs);
    }
    if (!header || flow_specs.empty())
        tw_error(TW_LOC, "flow CSV contains no flows");
    std::sort(flow_specs.begin(), flow_specs.end(), [](const flow_spec& a, const flow_spec& b) {
        if (a.start_seconds != b.start_seconds)
            return a.start_seconds < b.start_seconds;
        return a.flow_id < b.flow_id;
    });
}

static void finalize_units() {
    simulation_rate_unit =
        (units_seen.saw_gbps && !units_seen.saw_mbps) ? RATE_UNIT_GBPS : RATE_UNIT_MBPS;
    simulation_data_unit = (units_seen.saw_gb && !units_seen.saw_mb) ? DATA_UNIT_GB : DATA_UNIT_MB;
    output_time_unit = (units_seen.saw_s && !units_seen.saw_ms) ? TIME_UNIT_S : TIME_UNIT_MS;
    for (auto& sw : switches) {
        sw.terminal_bandwidth = trim_rate_from_mbps(sw.terminal_bandwidth);
        sw.switch_buffer = trim_data_from_mb(sw.switch_buffer);
        for (auto& l : sw.links)
            l.bandwidth = trim_rate_from_mbps(l.bandwidth);
    }
    for (auto& f : flow_specs)
        f.size_data = trim_data_from_mb(f.size_data);
}

static void compute_routes() {
    int n = (int)switches.size();
    next_switch_table.assign(n, std::vector<int>(n, -1));
    for (int src = 0; src < n; ++src) {
        std::vector<int> prev(n, -1), seen(n, 0);
        std::queue<int> q;
        seen[src] = 1;
        q.push(src);
        while (!q.empty()) {
            int u = q.front();
            q.pop();
            for (const auto& l : switches[u].links) {
                int v = l.dst_switch;
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
            if (!seen[dst])
                continue;
            int cur = dst, parent = prev[cur];
            while (parent >= 0 && parent != src) {
                cur = parent;
                parent = prev[cur];
            }
            next_switch_table[src][dst] = cur;
        }
    }
}

static void validate_topology_and_flows() {
    for (size_t switch_id = 0; switch_id < switches.size(); ++switch_id) {
        const raw_switch_info& sw = switches[switch_id];
        const size_t port_count = sw.links.size() + static_cast<size_t>(sw.terminal_count);
        if (port_count > MAX_PORTS_PER_SWITCH) {
            tw_error(TW_LOC, "switch %s requires %zu output ports; MAX_PORTS_PER_SWITCH=%d",
                     sw.name.c_str(), port_count, MAX_PORTS_PER_SWITCH);
        }

        size_t ingress_count = static_cast<size_t>(sw.terminal_count);
        for (const auto& upstream : switches) {
            for (const auto& link : upstream.links) {
                if (link.dst_switch == static_cast<int>(switch_id)) {
                    ++ingress_count;
                    break;
                }
            }
        }
        if (ingress_count > MAX_INGRESS_LINKS) {
            tw_error(TW_LOC, "switch %s requires %zu ingress links; MAX_INGRESS_LINKS=%d",
                     sw.name.c_str(), ingress_count, MAX_INGRESS_LINKS);
        }
        if (sw.terminal_bandwidth <= EPS)
            tw_error(TW_LOC, "switch %s terminal_bandwidth must be positive", sw.name.c_str());
        if (sw.switch_buffer <= EPS)
            tw_error(TW_LOC, "switch %s switch_buffer must be positive", sw.name.c_str());
        for (const auto& link : sw.links) {
            if (link.bandwidth <= EPS) {
                tw_error(TW_LOC, "switch %s has a non-positive link bandwidth", sw.name.c_str());
            }
        }
    }

    for (const flow_spec& flow : flow_specs) {
        const int src_switch = terminals[flow.source_terminal].switch_id;
        const int dst_switch = terminals[flow.destination_terminal].switch_id;
        if (src_switch != dst_switch && next_switch_table[src_switch][dst_switch] < 0) {
            tw_error(TW_LOC, "flow %llu has no route from terminal %d to terminal %d", flow.flow_id,
                     flow.source_terminal, flow.destination_terminal);
        }
    }
}

static void read_relpath(const char* section, const char* key, char* dst, size_t n) {
    char tmp[1024] = {0};
    if (configuration_get_value_relpath(&config, section, key, nullptr, tmp, sizeof(tmp)) > 0)
        std::snprintf(dst, n, "%s", tmp);
}
static void read_string(const char* section, const char* key, char* dst, size_t n) {
    char tmp[1024] = {0};
    if (configuration_get_value(&config, section, key, nullptr, tmp, sizeof(tmp)) > 0)
        std::snprintf(dst, n, "%s", tmp);
}
static void read_int(const char* section, const char* key, int* v) {
    int x = 0;
    if (configuration_get_value_int(&config, section, key, nullptr, &x) == 0)
        *v = x;
}
static void read_double(const char* section, const char* key, double* v) {
    double x = 0;
    if (configuration_get_value_double(&config, section, key, nullptr, &x) == 0)
        *v = x;
}
static void read_time(const char* section, const char* key, double* seconds) {
    char tmp[128] = {0};
    if (configuration_get_value(&config, section, key, nullptr, tmp, sizeof(tmp)) > 0)
        *seconds = parse_time_to_seconds(tmp, false);
}

static void load_config() {
    units_seen = unit_observation{};
    read_relpath("RATE_FLUID_WAN", "topology_file", cfg.topology_file, sizeof(cfg.topology_file));
    read_relpath("RATE_FLUID_WAN", "flow_file", cfg.flow_file, sizeof(cfg.flow_file));
    read_string("RATE_FLUID_WAN", "flow_summary_file", cfg.flow_summary_file,
                sizeof(cfg.flow_summary_file));
    read_time("RATE_FLUID_WAN", "link_latency", &cfg.link_latency_seconds);
    read_int("RATE_FLUID_WAN", "pause_enabled", &cfg.pause_enabled);
    read_double("RATE_FLUID_WAN", "pause_high_watermark_fraction",
                &cfg.pause_high_watermark_fraction);
    read_double("RATE_FLUID_WAN", "pause_low_watermark_fraction",
                &cfg.pause_low_watermark_fraction);
    read_int("RATE_FLUID_WAN", "debug", &cfg.debug);
    if (!cfg.topology_file[0] || !cfg.flow_file[0])
        tw_error(TW_LOC, "RATE_FLUID_WAN requires topology_file and flow_file");
    if (cfg.link_latency_seconds <= 0.0)
        tw_error(TW_LOC, "link_latency must be positive");
    if (!(cfg.pause_low_watermark_fraction >= 0.0 &&
          cfg.pause_low_watermark_fraction < cfg.pause_high_watermark_fraction &&
          cfg.pause_high_watermark_fraction <= 1.0))
        tw_error(TW_LOC, "PAUSE watermarks must satisfy 0 <= low < high <= 1");
    if (cfg.pause_enabled != 0 && cfg.pause_enabled != 1)
        tw_error(TW_LOC, "pause_enabled must be 0 or 1");
    load_topology_yaml(cfg.topology_file);
    load_flow_csv(cfg.flow_file);
    finalize_units();
    compute_routes();
    validate_topology_and_flows();
}

static tw_lpid get_switch_gid(int id) {
    tw_lpid gid = 0;
    codes_mapping_get_lp_id(GROUP_NAME, SWITCH_LP_NAME, nullptr, 1, 0, id, &gid);
    return gid;
}
static tw_lpid get_terminal_gid(int id) {
    tw_lpid gid = 0;
    codes_mapping_get_lp_id(GROUP_NAME, TERMINAL_LP_NAME, nullptr, 1, 0, id, &gid);
    return gid;
}

struct ingress_desc {
    int is_terminal = 0;
    int peer_index = -1;
};
struct port_desc {
    int is_terminal = 0;
    int target_index = -1;
    double capacity_rate = 0.0;
    int paused = 0;
};

struct switch_flow {
    int valid = 0;
    unsigned long long flow_id = 0;
    int source_terminal = -1;
    int destination_terminal = -1;
    int ingress_is_terminal = 0;
    int ingress_peer = -1;
    int port_id = -1;
    int downstream_ready = 0;
    int source_closed = 0;
    int pause_received = 0;
    int pause_sent = 0;
    double path_cap_rate = INF_RATE;
    /* Independent upstream supply bound, excluding this switch and its downstream. */
    double upstream_offer_rate = INF_RATE;
    double local_offer_rate = 0.0;
    double downstream_cap_rate = INF_RATE;
    double control_rate = 0.0;
    double incoming_rate = 0.0;
    double outgoing_rate = 0.0;
    double queued_data = 0.0;
    double total_data = 0.0;
    tw_stime last_upstream_flow_event_arrival_ns = -1.0;
    tw_stime last_downstream_flow_event_arrival_ns = -1.0;
};

struct switch_state {
    int switch_id = -1;
    int num_ports = 0;
    port_desc ports[MAX_PORTS_PER_SWITCH];
    int num_ingress = 0;
    ingress_desc ingress[MAX_INGRESS_LINKS];
    int flow_count = 0;
    switch_flow flows[MAX_FLOWS_PER_LP];
    double buffer_capacity = 0.0;
    double high_watermark = 0.0;
    double low_watermark = 0.0;
    int pause_active = 0;
    tw_stime last_update_ns = 0.0;
    tw_stime next_threshold_ns = -1.0;
    double max_buffer_seen = 0.0;
    double dropped_data = 0.0;
};

struct terminal_flow {
    int valid = 0;
    unsigned long long flow_id = 0;
    int source_terminal = -1;
    int destination_terminal = -1;
    double total_data = 0.0;
    double remaining_data = 0.0;
    double allowed_rate = 0.0;
    double send_rate = 0.0;
    double receive_rate = 0.0;
    double received_data = 0.0;
    int paused = 0;
    int source_complete = 0;
    int close_received = 0;
    int destination_complete = 0;
    tw_stime last_source_update_ns = 0.0;
    tw_stime last_dest_update_ns = 0.0;
    tw_stime predicted_complete_ns = -1.0;
    tw_stime start_ns = 0.0;
    tw_stime finish_ns = -1.0;
    tw_stime last_downstream_flow_event_arrival_ns = -1.0;
};

struct terminal_state {
    int terminal_id = -1;
    int attached_switch = -1;
    int flow_count = 0;
    terminal_flow flows[MAX_FLOWS_PER_LP];
};

enum event_type {
    FLOW_START = 1,
    FLOW_REGISTER = 2,
    DATA_RATE_UPDATE = 3,
    RATE_FEEDBACK = 4,
    BUFFER_THRESHOLD = 5,
    PAUSE_UPDATE = 6,
    FLOW_COMPLETE = 7,
    QUEUE_DRAIN = 8,
};

enum threshold_kind { THRESHOLD_NONE = 0, THRESHOLD_HIGH = 1, THRESHOLD_LOW = 2 };

struct rate_msg {
    int event_type = 0;
    unsigned long long flow_id = 0;
    int source_terminal = -1;
    int destination_terminal = -1;
    int source_switch = -1;
    int pause_asserted = 0;
    int pause_source_switch = -1;
    int threshold_kind = 0;
    int threshold_flow_index = -1;
    double rate = 0.0;
    double path_cap_rate = 0.0;
    double upstream_offer_rate = 0.0;
    double total_data = 0.0;
    tw_stime expected_time_ns = -1.0;

    /* Minimal reverse-computation metadata. */
    int rc_noop = 0;
    int rc_flow_index = -1;
    int rc_flow_created = 0;
    int rc_prev_flow_count = 0;
    switch_flow rc_switch_flow_before;
    terminal_flow rc_terminal_flow_before;
    tw_stime rc_prev_last_update_ns = 0.0;
    tw_stime rc_prev_next_threshold_ns = -1.0;
    int rc_prev_pause_active = 0;
    double rc_prev_max_buffer_seen = 0.0;
    double rc_prev_dropped_data = 0.0;
    double rc_dt_seconds = 0.0;
    double rc_prev_queues[MAX_FLOWS_PER_LP];
    double rc_prev_control_rates[MAX_FLOWS_PER_LP];
    double rc_prev_local_offer_rates[MAX_FLOWS_PER_LP];
    double rc_prev_outgoing_rates[MAX_FLOWS_PER_LP];
    int rc_prev_pause_received[MAX_FLOWS_PER_LP];
    int rc_prev_pause_sent[MAX_FLOWS_PER_LP];
    tw_stime rc_prev_last_upstream_flow_event_arrival_ns[MAX_FLOWS_PER_LP];
    tw_stime rc_prev_last_downstream_flow_event_arrival_ns[MAX_FLOWS_PER_LP];
    int rc_prev_port_paused[MAX_PORTS_PER_SWITCH];
    int rc_terminal_multi = 0;
    double rc_terminal_remaining[MAX_FLOWS_PER_LP];
    double rc_terminal_send_rate[MAX_FLOWS_PER_LP];
    double rc_terminal_predicted_complete[MAX_FLOWS_PER_LP];
    tw_stime rc_terminal_last_source_update[MAX_FLOWS_PER_LP];
    int rc_terminal_paused[MAX_FLOWS_PER_LP];
};

static_assert(std::is_trivially_copyable<rate_msg>::value, "rate_msg must be trivially copyable");
static_assert(std::is_trivially_copyable<switch_state>::value,
              "switch_state must be trivially copyable");
static_assert(std::is_trivially_copyable<terminal_state>::value,
              "terminal_state must be trivially copyable");

static const char* event_name(int e) {
    switch (e) {
    case FLOW_START:
        return "FLOW START";
    case FLOW_REGISTER:
        return "FLOW REGISTER";
    case DATA_RATE_UPDATE:
        return "DATA RATE UPDATE";
    case RATE_FEEDBACK:
        return "RATE FEEDBACK";
    case BUFFER_THRESHOLD:
        return "BUFFER THRESHOLD";
    case PAUSE_UPDATE:
        return "PAUSE UPDATE";
    case FLOW_COMPLETE:
        return "FLOW COMPLETE";
    case QUEUE_DRAIN:
        return "QUEUE DRAIN";
    default:
        return "UNKNOWN";
    }
}

static void debug_terminal_event(const terminal_state* ns, const rate_msg* m, tw_lp* lp) {
    if (!cfg.debug)
        return;
    std::printf("%s(F%llu) at terminal %s at %.6f %s", event_name(m->event_type),
                (unsigned long long)m->flow_id, terminals[ns->terminal_id].name.c_str(),
                seconds_to_output_time(ns_to_seconds(tw_now(lp))), time_unit_name());
    if (m->event_type == FLOW_REGISTER || m->event_type == DATA_RATE_UPDATE ||
        m->event_type == RATE_FEEDBACK)
        std::printf(" rate=%.6f %s", m->rate, rate_unit_name());
    if (m->event_type == PAUSE_UPDATE)
        std::printf(" state=%s", m->pause_asserted ? "PAUSE" : "RESUME");
    std::printf("\n");
    std::fflush(stdout);
}

static const char* threshold_name(int kind) {
    switch (kind) {
    case THRESHOLD_HIGH:
        return "HIGH";
    case THRESHOLD_LOW:
        return "LOW";
    default:
        return "UNKNOWN";
    }
}

static void debug_switch_event(const switch_state* ns, const rate_msg* m, tw_lp* lp) {
    if (!cfg.debug)
        return;

    if (m->event_type == BUFFER_THRESHOLD) {
        std::printf("BUFFER THRESHOLD at switch %s at %.6f %s threshold=%s\n",
                    switches[ns->switch_id].name.c_str(),
                    seconds_to_output_time(ns_to_seconds(tw_now(lp))), time_unit_name(),
                    threshold_name(m->threshold_kind));
        std::fflush(stdout);
        return;
    }
    std::printf("%s(F%llu) at switch %s at %.6f %s", event_name(m->event_type),
                (unsigned long long)m->flow_id, switches[ns->switch_id].name.c_str(),
                seconds_to_output_time(ns_to_seconds(tw_now(lp))), time_unit_name());
    if (m->event_type == FLOW_REGISTER || m->event_type == DATA_RATE_UPDATE ||
        m->event_type == RATE_FEEDBACK)
        std::printf(" rate=%.6f %s", m->rate, rate_unit_name());
    if (m->event_type == PAUSE_UPDATE)
        std::printf(" state=%s", m->pause_asserted ? "PAUSE" : "RESUME");
    std::printf("\n");
    std::fflush(stdout);
}

static int find_switch_flow(const switch_state* ns, unsigned long long id) {
    for (int i = 0; i < ns->flow_count; ++i)
        if (ns->flows[i].valid && ns->flows[i].flow_id == id)
            return i;
    return -1;
}
static int find_terminal_flow(const terminal_state* ns, unsigned long long id) {
    for (int i = 0; i < ns->flow_count; ++i)
        if (ns->flows[i].valid && ns->flows[i].flow_id == id)
            return i;
    return -1;
}
static int find_switch_port_to_switch(const switch_state* ns, int dst) {
    for (int p = 0; p < ns->num_ports; ++p)
        if (!ns->ports[p].is_terminal && ns->ports[p].target_index == dst)
            return p;
    return -1;
}
static int find_switch_port_to_terminal(const switch_state* ns, int dst) {
    for (int p = 0; p < ns->num_ports; ++p)
        if (ns->ports[p].is_terminal && ns->ports[p].target_index == dst)
            return p;
    return -1;
}

static int output_port_for_destination(const switch_state* ns, int destination_terminal) {
    int dst_sw = terminals[destination_terminal].switch_id;
    if (dst_sw == ns->switch_id)
        return find_switch_port_to_terminal(ns, destination_terminal);
    int next = next_switch_table[ns->switch_id][dst_sw];
    return next < 0 ? -1 : find_switch_port_to_switch(ns, next);
}

static double switch_buffer_total(const switch_state* ns) {
    double q = 0.0;
    for (int i = 0; i < ns->flow_count; ++i)
        q += std::max(0.0, ns->flows[i].queued_data);
    return q;
}

static void snapshot_switch_for_rc(switch_state* ns, rate_msg* m) {
    m->rc_prev_last_update_ns = ns->last_update_ns;
    m->rc_prev_next_threshold_ns = ns->next_threshold_ns;
    m->rc_prev_pause_active = ns->pause_active;
    m->rc_prev_max_buffer_seen = ns->max_buffer_seen;
    m->rc_prev_dropped_data = ns->dropped_data;
    m->rc_prev_flow_count = ns->flow_count;
    for (int i = 0; i < ns->flow_count; ++i) {
        m->rc_prev_queues[i] = ns->flows[i].queued_data;
        m->rc_prev_control_rates[i] = ns->flows[i].control_rate;
        m->rc_prev_local_offer_rates[i] = ns->flows[i].local_offer_rate;
        m->rc_prev_outgoing_rates[i] = ns->flows[i].outgoing_rate;
        m->rc_prev_pause_received[i] = ns->flows[i].pause_received;
        m->rc_prev_pause_sent[i] = ns->flows[i].pause_sent;
        m->rc_prev_last_upstream_flow_event_arrival_ns[i] =
            ns->flows[i].last_upstream_flow_event_arrival_ns;
        m->rc_prev_last_downstream_flow_event_arrival_ns[i] =
            ns->flows[i].last_downstream_flow_event_arrival_ns;
    }
    for (int p = 0; p < ns->num_ports; ++p)
        m->rc_prev_port_paused[p] = ns->ports[p].paused;
}

static void restore_switch_from_rc(switch_state* ns, rate_msg* m) {
    ns->last_update_ns = m->rc_prev_last_update_ns;
    ns->next_threshold_ns = m->rc_prev_next_threshold_ns;
    ns->pause_active = m->rc_prev_pause_active;
    ns->max_buffer_seen = m->rc_prev_max_buffer_seen;
    ns->dropped_data = m->rc_prev_dropped_data;
    for (int i = 0; i < m->rc_prev_flow_count; ++i) {
        ns->flows[i].queued_data = m->rc_prev_queues[i];
        ns->flows[i].control_rate = m->rc_prev_control_rates[i];
        ns->flows[i].local_offer_rate = m->rc_prev_local_offer_rates[i];
        ns->flows[i].outgoing_rate = m->rc_prev_outgoing_rates[i];
        ns->flows[i].pause_received = m->rc_prev_pause_received[i];
        ns->flows[i].pause_sent = m->rc_prev_pause_sent[i];
        ns->flows[i].last_upstream_flow_event_arrival_ns =
            m->rc_prev_last_upstream_flow_event_arrival_ns[i];
        ns->flows[i].last_downstream_flow_event_arrival_ns =
            m->rc_prev_last_downstream_flow_event_arrival_ns[i];
    }
    for (int p = 0; p < ns->num_ports; ++p)
        ns->ports[p].paused = m->rc_prev_port_paused[p];
    if (m->rc_flow_created) {
        ns->flow_count = m->rc_prev_flow_count;
        if (ns->flow_count < MAX_FLOWS_PER_LP)
            std::memset(&ns->flows[ns->flow_count], 0, sizeof(ns->flows[ns->flow_count]));
    } else if (m->rc_flow_index >= 0) {
        ns->flows[m->rc_flow_index] = m->rc_switch_flow_before;
    }
}

static void advance_switch_state(switch_state* ns, tw_stime now_ns) {
    if (now_ns <= ns->last_update_ns) {
        ns->last_update_ns = now_ns;
        return;
    }
    const double occupancy_before = switch_buffer_total(ns);
    double dt = ns_to_seconds(now_ns - ns->last_update_ns);
    double additions[MAX_FLOWS_PER_LP] = {0.0};
    double total_positive_addition = 0.0;
    for (int i = 0; i < ns->flow_count; ++i) {
        switch_flow& f = ns->flows[i];
        if (!f.valid)
            continue;
        const double before = f.queued_data;
        double dq = rate_to_data_per_second(f.incoming_rate - f.outgoing_rate) * dt;
        f.queued_data += dq;
        if (f.queued_data < EPS)
            f.queued_data = 0.0;
        additions[i] = std::max(0.0, f.queued_data - before);
        total_positive_addition += additions[i];
    }

    /* PAUSE is advisory and takes one link latency to propagate.  If the
     * in-flight excess still fills the physical shared buffer, drop only the
     * overflow volume and distribute that loss across flows that added queue
     * during this elapsed-time segment. */
    double total = switch_buffer_total(ns);
    if (ns->buffer_capacity > EPS && total > ns->buffer_capacity + EPS) {
        double excess = total - ns->buffer_capacity;
        ns->dropped_data += excess;
        if (total_positive_addition > EPS) {
            for (int i = 0; i < ns->flow_count; ++i) {
                if (additions[i] <= EPS)
                    continue;
                double remove = excess * (additions[i] / total_positive_addition);
                ns->flows[i].queued_data = std::max(0.0, ns->flows[i].queued_data - remove);
            }
        } else if (total > EPS) {
            const double scale = ns->buffer_capacity / total;
            for (int i = 0; i < ns->flow_count; ++i)
                ns->flows[i].queued_data *= scale;
        }
    }
    ns->last_update_ns = now_ns;
    const double occupancy_after = switch_buffer_total(ns);
    ns->max_buffer_seen = std::max(ns->max_buffer_seen, occupancy_after);
    if (cfg.debug && std::fabs(occupancy_after - occupancy_before) > EPS) {
        std::printf("BUFFER OCCUPANCY at switch %s at %.6f %s occupancy=%.6f %s capacity=%.6f %s\n",
                    switches[ns->switch_id].name.c_str(),
                    seconds_to_output_time(ns_to_seconds(now_ns)), time_unit_name(),
                    occupancy_after, data_unit_name(), ns->buffer_capacity, data_unit_name());
        std::fflush(stdout);
    }
}

static void maxmin_allocate(const std::vector<int>& idx, const std::vector<double>& demands,
                            double capacity, std::vector<double>* alloc) {
    alloc->assign(idx.size(), 0.0);
    std::vector<int> active(idx.size(), 1);
    double remaining = std::max(0.0, capacity);
    int count = (int)idx.size();
    while (count > 0 && remaining > EPS) {
        double share = remaining / count;
        bool fixed_any = false;
        for (size_t j = 0; j < idx.size(); ++j) {
            if (!active[j])
                continue;
            if (demands[j] < share - EPS) {
                (*alloc)[j] = std::max(0.0, demands[j]);
                remaining -= (*alloc)[j];
                active[j] = 0;
                --count;
                fixed_any = true;
            }
        }
        if (!fixed_any) {
            for (size_t j = 0; j < idx.size(); ++j)
                if (active[j])
                    (*alloc)[j] = share;
            break;
        }
    }
}

static void schedule_event(tw_lpid gid, int type, const rate_msg& src, double delay_seconds,
                           tw_lp* lp) {
    double delay_ns = std::max(seconds_to_ns(delay_seconds), g_tw_lookahead + 1.0);
    tw_event* e = tw_event_new(gid, delay_ns, lp);
    rate_msg* m = (rate_msg*)tw_event_data(e);
    std::memset(m, 0, sizeof(*m));
    m->event_type = type;
    m->flow_id = src.flow_id;
    m->source_terminal = src.source_terminal;
    m->destination_terminal = src.destination_terminal;
    m->source_switch = src.source_switch;
    m->rate = src.rate;
    m->path_cap_rate = src.path_cap_rate;
    m->upstream_offer_rate = src.upstream_offer_rate;
    m->total_data = src.total_data;
    m->pause_asserted = src.pause_asserted;
    m->pause_source_switch = src.pause_source_switch;
    m->threshold_kind = src.threshold_kind;
    m->threshold_flow_index = src.threshold_flow_index;
    m->expected_time_ns = src.expected_time_ns;
    tw_event_send(e);
}

/*
 * Per-flow state events use physical link latency plus a superdense-time
 * ordering epsilon when more than one such event for the same flow and
 * direction would otherwise arrive at the same timestamp.  The epsilon is
 * not modeled propagation delay; it only preserves the sender's causal
 * generation order at the receiver.
 */
static void schedule_ordered_flow_event(tw_lpid gid, int type, const rate_msg& src,
                                        double physical_delay_seconds, tw_stime* last_arrival_ns,
                                        tw_lp* lp) {
    const double physical_delay_ns =
        std::max(seconds_to_ns(physical_delay_seconds), g_tw_lookahead + 1.0);
    tw_stime target = tw_now(lp) + physical_delay_ns;
    if (*last_arrival_ns >= 0.0 && target <= *last_arrival_ns + 0.5)
        target = *last_arrival_ns + 1.0;
    *last_arrival_ns = target;
    schedule_event(gid, type, src, ns_to_seconds(target - tw_now(lp)), lp);
}

static void send_rate_feedback_upstream(switch_state* ns, switch_flow& f, double rate, tw_lp* lp) {
    rate_msg out{};
    out.flow_id = f.flow_id;
    out.source_terminal = f.source_terminal;
    out.destination_terminal = f.destination_terminal;
    out.source_switch = ns->switch_id;
    out.rate = rate;
    out.total_data = f.total_data;
    tw_lpid gid =
        f.ingress_is_terminal ? get_terminal_gid(f.ingress_peer) : get_switch_gid(f.ingress_peer);
    schedule_ordered_flow_event(gid, RATE_FEEDBACK, out, cfg.link_latency_seconds,
                                &f.last_upstream_flow_event_arrival_ns, lp);
}

static void send_data_update_downstream(switch_state* ns, switch_flow& f, double rate, tw_lp* lp) {
    const port_desc& p = ns->ports[f.port_id];
    rate_msg out{};
    out.flow_id = f.flow_id;
    out.source_terminal = f.source_terminal;
    out.destination_terminal = f.destination_terminal;
    out.source_switch = ns->switch_id;
    out.rate = rate;
    out.upstream_offer_rate = f.local_offer_rate;
    out.total_data = f.total_data;
    tw_lpid gid = p.is_terminal ? get_terminal_gid(p.target_index) : get_switch_gid(p.target_index);

    schedule_ordered_flow_event(gid, DATA_RATE_UPDATE, out, cfg.link_latency_seconds,
                                &f.last_downstream_flow_event_arrival_ns, lp);
}

static void recompute_switch(switch_state* ns, tw_lp* lp, bool emit,
                             int registering_flow_index = -1) {
    for (int p = 0; p < ns->num_ports; ++p) {
        std::vector<int> ids;
        std::vector<double> offer_demands;
        std::vector<double> control_demands;
        for (int i = 0; i < ns->flow_count; ++i) {
            switch_flow& f = ns->flows[i];
            if (!f.valid || f.port_id != p || f.source_closed)
                continue;
            ids.push_back(i);
            /* Supply upstream of this port, independent of this port's
             * previously advertised rate and all downstream feedback. */
            const double upstream = std::min(f.path_cap_rate, f.upstream_offer_rate);
            offer_demands.push_back(upstream);
            const double downstream_cap = f.downstream_ready ? f.downstream_cap_rate : INF_RATE;
            control_demands.push_back(std::min(upstream, downstream_cap));
        }
        std::map<int, double> previous_offers;
        for (int i : ids)
            previous_offers[i] = ns->flows[i].local_offer_rate;
        std::vector<double> offer_alloc;
        maxmin_allocate(ids, offer_demands, ns->ports[p].capacity_rate, &offer_alloc);
        std::vector<double> control_alloc;
        maxmin_allocate(ids, control_demands, ns->ports[p].capacity_rate, &control_alloc);
        for (size_t j = 0; j < ids.size(); ++j) {
            switch_flow& f = ns->flows[ids[j]];
            const double old_control = f.control_rate;
            f.local_offer_rate = offer_alloc[j];
            f.control_rate = control_alloc[j];
            if (emit && std::fabs(f.control_rate - old_control) > EPS)
                send_rate_feedback_upstream(ns, f, f.control_rate, lp);
        }

        ids.clear();
        std::vector<double> actual_demands;
        if (!ns->ports[p].paused) {
            for (int i = 0; i < ns->flow_count; ++i) {
                switch_flow& f = ns->flows[i];
                if (!f.valid || f.port_id != p || f.pause_received)
                    continue;
                double demand = f.queued_data > EPS ? INF_RATE : std::max(0.0, f.incoming_rate);
                if (demand <= EPS)
                    continue;
                ids.push_back(i);
                actual_demands.push_back(demand);
            }
        }
        std::vector<double> actual_alloc;
        maxmin_allocate(ids, actual_demands, ns->ports[p].paused ? 0.0 : ns->ports[p].capacity_rate,
                        &actual_alloc);
        std::map<int, double> new_rate;
        for (size_t j = 0; j < ids.size(); ++j)
            new_rate[ids[j]] = actual_alloc[j];
        for (int i = 0; i < ns->flow_count; ++i) {
            switch_flow& f = ns->flows[i];
            if (!f.valid || f.port_id != p)
                continue;
            double old = f.outgoing_rate;
            double now = new_rate.count(i) ? new_rate[i] : 0.0;
            f.outgoing_rate = now;
            const bool offer_changed =
                previous_offers.count(i) &&
                std::fabs(f.local_offer_rate - previous_offers[i]) > EPS;
            if (emit && i != registering_flow_index &&
                (std::fabs(now - old) > EPS || offer_changed))
                send_data_update_downstream(ns, f, now, lp);
        }
    }
}

static void schedule_next_threshold(switch_state* ns, tw_lp* lp) {
    double best = std::numeric_limits<double>::infinity();
    int event_type = 0;
    int threshold_kind = THRESHOLD_NONE;
    int queue_flow_index = -1;
    double total_q = switch_buffer_total(ns), total_dq = 0.0;

    /* Queue-drain timing is an internal rate-state event, not a shared-buffer
     * threshold.  Keep it separate so BUFFER_THRESHOLD means only HIGH/LOW. */
    for (int i = 0; i < ns->flow_count; ++i) {
        const switch_flow& f = ns->flows[i];
        if (!f.valid)
            continue;
        const double dq = rate_to_data_per_second(f.incoming_rate - f.outgoing_rate);
        total_dq += dq;
        if (f.queued_data > EPS && dq < -EPS) {
            const double t = f.queued_data / (-dq);
            if (t < best) {
                best = t;
                event_type = QUEUE_DRAIN;
                queue_flow_index = i;
                threshold_kind = THRESHOLD_NONE;
            }
        }
    }

    if (cfg.pause_enabled && !ns->pause_active && total_dq > EPS &&
        total_q < ns->high_watermark - EPS) {
        const double t = (ns->high_watermark - total_q) / total_dq;
        if (t < best) {
            best = t;
            event_type = BUFFER_THRESHOLD;
            threshold_kind = THRESHOLD_HIGH;
            queue_flow_index = -1;
        }
    }
    if (cfg.pause_enabled && ns->pause_active && total_dq < -EPS &&
        total_q > ns->low_watermark + EPS) {
        const double t = (total_q - ns->low_watermark) / (-total_dq);
        if (t < best) {
            best = t;
            event_type = BUFFER_THRESHOLD;
            threshold_kind = THRESHOLD_LOW;
            queue_flow_index = -1;
        }
    }

    if (!std::isfinite(best)) {
        ns->next_threshold_ns = -1.0;
        return;
    }
    const tw_stime target = tw_now(lp) + std::max(seconds_to_ns(best), g_tw_lookahead + 1.0);
    ns->next_threshold_ns = target;
    rate_msg out{};
    out.threshold_kind = threshold_kind;
    out.threshold_flow_index = queue_flow_index;
    if (event_type == QUEUE_DRAIN && queue_flow_index >= 0)
        out.flow_id = ns->flows[queue_flow_index].flow_id;
    out.expected_time_ns = target;
    schedule_event(lp->gid, event_type, out, ns_to_seconds(target - tw_now(lp)), lp);
}

static void send_pause_upstream(switch_state* ns, switch_flow& f, int asserted, tw_lp* lp) {
    rate_msg out{};
    out.flow_id = f.flow_id;
    out.source_terminal = f.source_terminal;
    out.destination_terminal = f.destination_terminal;
    out.pause_asserted = asserted;
    out.pause_source_switch = ns->switch_id;
    out.total_data = f.total_data;
    const tw_lpid gid =
        f.ingress_is_terminal ? get_terminal_gid(f.ingress_peer) : get_switch_gid(f.ingress_peer);
    schedule_ordered_flow_event(gid, PAUSE_UPDATE, out, cfg.link_latency_seconds,
                                &f.last_upstream_flow_event_arrival_ns, lp);
}

static void pause_high_rate_contributors(switch_state* ns, tw_lp* lp) {
    double total_dq = 0.0;
    struct candidate {
        int index;
        double rate;
    };
    std::vector<candidate> candidates;

    for (int i = 0; i < ns->flow_count; ++i) {
        switch_flow& f = ns->flows[i];
        if (!f.valid)
            continue;
        total_dq += rate_to_data_per_second(f.incoming_rate - f.outgoing_rate);
        if (!f.source_closed && !f.pause_sent && f.incoming_rate > EPS)
            candidates.push_back({i, f.incoming_rate});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const candidate& a, const candidate& b) { return a.rate > b.rate; });

    double remaining_growth = total_dq;
    for (const candidate& c : candidates) {
        if (remaining_growth <= EPS)
            break;
        switch_flow& f = ns->flows[c.index];
        f.pause_sent = 1;
        send_pause_upstream(ns, f, 1, lp);
        remaining_growth -= rate_to_data_per_second(f.incoming_rate);
    }
}

static void resume_paused_contributors(switch_state* ns, tw_lp* lp) {
    for (int i = 0; i < ns->flow_count; ++i) {
        switch_flow& f = ns->flows[i];
        if (!f.valid || !f.pause_sent)
            continue;
        f.pause_sent = 0;
        send_pause_upstream(ns, f, 0, lp);
    }
}

static void forward_flow_complete(switch_state* ns, switch_flow& f, tw_lp* lp) {
    const port_desc& p = ns->ports[f.port_id];
    rate_msg out{};
    out.flow_id = f.flow_id;
    out.source_terminal = f.source_terminal;
    out.destination_terminal = f.destination_terminal;
    out.source_switch = ns->switch_id;
    out.total_data = f.total_data;
    tw_lpid gid = p.is_terminal ? get_terminal_gid(p.target_index) : get_switch_gid(p.target_index);
    schedule_ordered_flow_event(gid, FLOW_COMPLETE, out, cfg.link_latency_seconds,
                                &f.last_downstream_flow_event_arrival_ns, lp);
}

static void switch_init(switch_state* ns, tw_lp* lp) {
    std::memset(ns, 0, sizeof(*ns));
    ns->switch_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    const raw_switch_info& sw = switches[ns->switch_id];
    ns->buffer_capacity = sw.switch_buffer;
    ns->high_watermark = ns->buffer_capacity * cfg.pause_high_watermark_fraction;
    ns->low_watermark = ns->buffer_capacity * cfg.pause_low_watermark_fraction;
    ns->next_threshold_ns = -1.0;
    ns->last_update_ns = 0.0;
    for (const auto& l : sw.links) {
        port_desc& p = ns->ports[ns->num_ports++];
        p.is_terminal = 0;
        p.target_index = l.dst_switch;
        p.capacity_rate = l.bandwidth;
    }
    for (int j = 0; j < sw.terminal_count; ++j) {
        port_desc& p = ns->ports[ns->num_ports++];
        p.is_terminal = 1;
        p.target_index = sw.terminal_start + j;
        p.capacity_rate = sw.terminal_bandwidth;
        ingress_desc& in = ns->ingress[ns->num_ingress++];
        in.is_terminal = 1;
        in.peer_index = sw.terminal_start + j;
    }
    for (int u = 0; u < (int)switches.size(); ++u) {
        for (const auto& l : switches[u].links)
            if (l.dst_switch == ns->switch_id) {
                ingress_desc& in = ns->ingress[ns->num_ingress++];
                in.is_terminal = 0;
                in.peer_index = u;
                break;
            }
    }
}

static int create_switch_flow(switch_state* ns, const rate_msg* m) {
    if (ns->flow_count >= MAX_FLOWS_PER_LP)
        tw_error(TW_LOC, "switch %d exceeded MAX_FLOWS_PER_LP", ns->switch_id);
    int i = ns->flow_count++;
    switch_flow& f = ns->flows[i];
    std::memset(&f, 0, sizeof(f));
    f.valid = 1;
    f.flow_id = m->flow_id;
    f.source_terminal = m->source_terminal;
    f.destination_terminal = m->destination_terminal;
    f.total_data = m->total_data;
    f.last_upstream_flow_event_arrival_ns = -1.0;
    f.last_downstream_flow_event_arrival_ns = -1.0;
    f.ingress_is_terminal = (m->source_switch < 0);
    f.ingress_peer = f.ingress_is_terminal ? m->source_terminal : m->source_switch;
    f.port_id = output_port_for_destination(ns, m->destination_terminal);
    if (f.port_id < 0)
        tw_error(TW_LOC, "switch %d has no route to terminal %d", ns->switch_id,
                 m->destination_terminal);
    f.path_cap_rate = m->path_cap_rate > EPS ? m->path_cap_rate : INF_RATE;
    f.upstream_offer_rate = std::max(0.0, m->upstream_offer_rate);
    f.incoming_rate = std::max(0.0, m->rate);
    f.downstream_ready = 0;
    f.downstream_cap_rate = INF_RATE;
    /*
     * FLOW_REGISTER carries the best physical path cap discovered so far.
     * The source-facing switch must always return the first currently known
     * allowance so the source can begin sending immediately. Intermediate
     * switches seed control_rate with the already-known path cap and only
     * feed back a newly tighter constraint.
     */
    f.control_rate = f.ingress_is_terminal ? 0.0 : f.path_cap_rate;
    return i;
}

static void switch_event(switch_state* ns, tw_bf*, rate_msg* m, tw_lp* lp) {
    m->rc_noop = 0;
    m->rc_flow_index = -1;
    m->rc_flow_created = 0;
    snapshot_switch_for_rc(ns, m);
    advance_switch_state(ns, tw_now(lp));

    if (m->event_type == FLOW_REGISTER) {
        int i = find_switch_flow(ns, m->flow_id);
        if (i < 0) {
            i = create_switch_flow(ns, m);
            m->rc_flow_created = 1;
        }
        m->rc_flow_index = i;
        m->rc_switch_flow_before = ns->flows[i];
        switch_flow& f = ns->flows[i];
        debug_switch_event(ns, m, lp);
        recompute_switch(ns, lp, true, i);
        const port_desc& p = ns->ports[f.port_id];

        /*
         * FLOW_REGISTER is also the first downstream data-rate indication.
         * Carry the actual rate currently transmitted on this hop, while
         * path_cap_rate independently carries the tightest physical link
         * capacity discovered so far.
         */
        rate_msg out = *m;
        out.source_switch = ns->switch_id;
        out.rate = f.outgoing_rate;
        out.upstream_offer_rate = f.local_offer_rate;
        out.path_cap_rate = std::min(f.path_cap_rate, p.capacity_rate);
        const tw_lpid gid =
            p.is_terminal ? get_terminal_gid(p.target_index) : get_switch_gid(p.target_index);
        schedule_ordered_flow_event(gid, FLOW_REGISTER, out, cfg.link_latency_seconds,
                                    &f.last_downstream_flow_event_arrival_ns, lp);
    } else if (m->event_type == RATE_FEEDBACK) {
        int i = find_switch_flow(ns, m->flow_id);
        if (i < 0)
            tw_error(TW_LOC, "RATE_FEEDBACK for unknown flow %llu", m->flow_id);
        m->rc_flow_index = i;
        m->rc_switch_flow_before = ns->flows[i];
        debug_switch_event(ns, m, lp);
        ns->flows[i].downstream_ready = 1;
        ns->flows[i].downstream_cap_rate = std::max(0.0, m->rate);
        recompute_switch(ns, lp, true);
    } else if (m->event_type == DATA_RATE_UPDATE) {
        int i = find_switch_flow(ns, m->flow_id);
        if (i < 0)
            tw_error(TW_LOC, "DATA_RATE_UPDATE for unknown flow %llu", m->flow_id);
        m->rc_flow_index = i;
        m->rc_switch_flow_before = ns->flows[i];
        debug_switch_event(ns, m, lp);
        ns->flows[i].incoming_rate = std::max(0.0, m->rate);
        ns->flows[i].upstream_offer_rate = std::max(0.0, m->upstream_offer_rate);
        recompute_switch(ns, lp, true);
    } else if (m->event_type == PAUSE_UPDATE) {
        int i = find_switch_flow(ns, m->flow_id);
        if (i < 0)
            tw_error(TW_LOC, "PAUSE_UPDATE for unknown flow %llu", m->flow_id);
        m->rc_flow_index = i;
        m->rc_switch_flow_before = ns->flows[i];
        switch_flow& f = ns->flows[i];
        debug_switch_event(ns, m, lp);
        f.pause_received = m->pause_asserted ? 1 : 0;
        recompute_switch(ns, lp, true);
        send_pause_upstream(ns, f, m->pause_asserted, lp);
    } else if (m->event_type == BUFFER_THRESHOLD) {
        if (m->expected_time_ns >= 0.0 &&
            std::fabs((double)(m->expected_time_ns - ns->next_threshold_ns)) > 0.5) {
            return;
        }
        ns->next_threshold_ns = -1.0;
        debug_switch_event(ns, m, lp);
        if (m->threshold_kind == THRESHOLD_HIGH && !ns->pause_active) {
            ns->pause_active = 1;
            pause_high_rate_contributors(ns, lp);
        } else if (m->threshold_kind == THRESHOLD_LOW && ns->pause_active) {
            ns->pause_active = 0;
            resume_paused_contributors(ns, lp);
        }
        recompute_switch(ns, lp, true);
    } else if (m->event_type == QUEUE_DRAIN) {
        if (m->expected_time_ns >= 0.0 &&
            std::fabs((double)(m->expected_time_ns - ns->next_threshold_ns)) > 0.5) {
            return;
        }
        ns->next_threshold_ns = -1.0;
        debug_switch_event(ns, m, lp);

        /* A per-flow queue can drain at exactly the same instant that the
         * shared buffer reaches the PAUSE low watermark.  schedule_next_threshold()
         * keeps only the earliest predicted switch event, so QUEUE_DRAIN can win
         * that tie.  In that case the LOW BUFFER_THRESHOLD is no longer
         * schedulable after advance_switch_state() has moved occupancy exactly to
         * the watermark.  Honor the coincident LOW crossing here so paused
         * upstream contributors always receive their RESUME. */
        if (cfg.pause_enabled && ns->pause_active &&
            switch_buffer_total(ns) <= ns->low_watermark + EPS) {
            ns->pause_active = 0;
            resume_paused_contributors(ns, lp);
        }

        recompute_switch(ns, lp, true);
    } else if (m->event_type == FLOW_COMPLETE) {
        int i = find_switch_flow(ns, m->flow_id);
        if (i < 0)
            tw_error(TW_LOC, "FLOW_COMPLETE for unknown flow %llu", m->flow_id);
        m->rc_flow_index = i;
        m->rc_switch_flow_before = ns->flows[i];
        debug_switch_event(ns, m, lp);
        ns->flows[i].source_closed = 1;
        recompute_switch(ns, lp, true);
        forward_flow_complete(ns, ns->flows[i], lp);
    } else {
        tw_error(TW_LOC, "switch received unknown event %d", m->event_type);
    }
    schedule_next_threshold(ns, lp);
}

static void switch_rev_event(switch_state* ns, tw_bf*, rate_msg* m, tw_lp*) {
    if (m->rc_noop)
        return;
    restore_switch_from_rc(ns, m);
}

static void switch_finalize(switch_state* ns, tw_lp*) {
    if (cfg.debug)
        std::printf("FINAL switch %s max_buffer=%.6f %s dropped=%.6f %s\n",
                    switches[ns->switch_id].name.c_str(), ns->max_buffer_seen, data_unit_name(),
                    ns->dropped_data, data_unit_name());
}

static void advance_source_flow(terminal_flow& f, tw_stime now) {
    if (now <= f.last_source_update_ns) {
        f.last_source_update_ns = now;
        return;
    }
    double dt = ns_to_seconds(now - f.last_source_update_ns);
    f.remaining_data -= rate_to_data_per_second(f.send_rate) * dt;
    if (f.remaining_data < EPS)
        f.remaining_data = 0.0;
    f.last_source_update_ns = now;
}
static void advance_dest_flow(terminal_flow& f, tw_stime now) {
    if (now <= f.last_dest_update_ns) {
        f.last_dest_update_ns = now;
        return;
    }
    double dt = ns_to_seconds(now - f.last_dest_update_ns);
    f.received_data += rate_to_data_per_second(f.receive_rate) * dt;
    if (f.received_data > f.total_data && f.received_data - f.total_data < 1e-6)
        f.received_data = f.total_data;
    f.last_dest_update_ns = now;
}

static int create_terminal_flow(terminal_state* ns, const rate_msg* m, tw_stime now) {
    if (ns->flow_count >= MAX_FLOWS_PER_LP)
        tw_error(TW_LOC, "terminal %d exceeded MAX_FLOWS_PER_LP", ns->terminal_id);
    int i = ns->flow_count++;
    terminal_flow& f = ns->flows[i];
    std::memset(&f, 0, sizeof(f));
    f.valid = 1;
    f.flow_id = m->flow_id;
    f.source_terminal = m->source_terminal;
    f.destination_terminal = m->destination_terminal;
    f.total_data = m->total_data;
    f.remaining_data = m->total_data;
    f.start_ns = now;
    f.last_source_update_ns = now;
    f.last_dest_update_ns = now;
    f.predicted_complete_ns = -1.0;
    f.finish_ns = -1.0;
    f.last_downstream_flow_event_arrival_ns = -1.0;
    return i;
}

static void send_source_data_rate(terminal_state* ns, terminal_flow& f, double rate, tw_lp* lp) {
    rate_msg out{};
    out.flow_id = f.flow_id;
    out.source_terminal = f.source_terminal;
    out.destination_terminal = f.destination_terminal;
    out.source_switch = -1;
    out.rate = rate;
    /* A source's independent supply bound is its access capacity, not
     * its sending rate (which may reflect feedback or PAUSE). */
    out.upstream_offer_rate = switches[ns->attached_switch].terminal_bandwidth;
    out.total_data = f.total_data;
    schedule_ordered_flow_event(get_switch_gid(ns->attached_switch), DATA_RATE_UPDATE, out,
                                cfg.link_latency_seconds, &f.last_downstream_flow_event_arrival_ns,
                                lp);
}

static void schedule_source_completion(terminal_flow& f, tw_lp* lp) {
    if (f.source_complete || f.send_rate <= EPS || f.remaining_data <= EPS) {
        f.predicted_complete_ns = -1.0;
        return;
    }
    double seconds = f.remaining_data / rate_to_data_per_second(f.send_rate);
    tw_stime target = tw_now(lp) + std::max(seconds_to_ns(seconds), g_tw_lookahead + 1.0);
    f.predicted_complete_ns = target;
    rate_msg out{};
    out.flow_id = f.flow_id;
    out.source_terminal = f.source_terminal;
    out.destination_terminal = f.destination_terminal;
    out.total_data = f.total_data;
    out.expected_time_ns = target;
    schedule_event(lp->gid, FLOW_COMPLETE, out, ns_to_seconds(target - tw_now(lp)), lp);
}

static void complete_source_flow(terminal_state* ns, terminal_flow& f, tw_lp* lp) {
    if (f.source_complete)
        return;
    f.remaining_data = 0.0;
    f.source_complete = 1;
    f.predicted_complete_ns = -1.0;
    if (f.send_rate > EPS) {
        f.send_rate = 0.0;
        send_source_data_rate(ns, f, 0.0, lp);
    }
    rate_msg out{};
    out.flow_id = f.flow_id;
    out.source_terminal = f.source_terminal;
    out.destination_terminal = f.destination_terminal;
    out.total_data = f.total_data;
    schedule_ordered_flow_event(get_switch_gid(ns->attached_switch), FLOW_COMPLETE, out,
                                cfg.link_latency_seconds, &f.last_downstream_flow_event_arrival_ns,
                                lp);
}

static void maybe_mark_destination_complete(terminal_flow& f, tw_stime now) {
    if (!f.destination_complete && f.close_received && f.receive_rate <= EPS) {
        f.destination_complete = 1;
        f.finish_ns = now;
    }
}

static void terminal_init(terminal_state* ns, tw_lp* lp) {
    std::memset(ns, 0, sizeof(*ns));
    ns->terminal_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    ns->attached_switch = terminals[ns->terminal_id].switch_id;
    for (const auto& spec : flow_specs) {
        if (spec.source_terminal != ns->terminal_id)
            continue;
        double delay = spec.start_seconds - ns_to_seconds(tw_now(lp));
        if (delay < 0.0)
            delay = 0.0;
        rate_msg out{};
        out.flow_id = spec.flow_id;
        out.source_terminal = spec.source_terminal;
        out.destination_terminal = spec.destination_terminal;
        out.total_data = spec.size_data;
        schedule_event(lp->gid, FLOW_START, out, delay, lp);
    }
}

static void terminal_event(terminal_state* ns, tw_bf*, rate_msg* m, tw_lp* lp) {
    m->rc_noop = 0;
    m->rc_flow_index = -1;
    m->rc_flow_created = 0;
    m->rc_prev_flow_count = ns->flow_count;
    int i = find_terminal_flow(ns, m->flow_id);

    if (m->event_type == FLOW_START) {
        if (i >= 0) {
            m->rc_noop = 1;
            return;
        }
        debug_terminal_event(ns, m, lp);
        i = create_terminal_flow(ns, m, tw_now(lp));
        m->rc_flow_created = 1;
        m->rc_flow_index = i;
        terminal_flow& f = ns->flows[i];
        const double initial_rate = switches[ns->attached_switch].terminal_bandwidth;
        f.allowed_rate = initial_rate;
        f.send_rate = initial_rate;

        rate_msg out = *m;
        out.source_switch = -1;
        out.rate = initial_rate;
        out.path_cap_rate = initial_rate;
        out.upstream_offer_rate = initial_rate;
        schedule_ordered_flow_event(get_switch_gid(ns->attached_switch), FLOW_REGISTER, out,
                                    cfg.link_latency_seconds,
                                    &f.last_downstream_flow_event_arrival_ns, lp);
        schedule_source_completion(f, lp);
    } else if (m->event_type == RATE_FEEDBACK) {
        if (i < 0)
            tw_error(TW_LOC, "terminal RATE_FEEDBACK for unknown flow %llu", m->flow_id);
        m->rc_flow_index = i;
        m->rc_terminal_flow_before = ns->flows[i];
        terminal_flow& f = ns->flows[i];
        debug_terminal_event(ns, m, lp);
        advance_source_flow(f, tw_now(lp));
        if (!f.source_complete && f.remaining_data <= 1e-6) {
            complete_source_flow(ns, f, lp);
            return;
        }
        f.allowed_rate = std::max(0.0, m->rate);
        double new_rate = f.paused || f.source_complete ? 0.0 : f.allowed_rate;
        if (std::fabs(new_rate - f.send_rate) > EPS) {
            f.send_rate = new_rate;
            send_source_data_rate(ns, f, new_rate, lp);
        }
        schedule_source_completion(f, lp);
    } else if (m->event_type == PAUSE_UPDATE) {
        if (i < 0)
            tw_error(TW_LOC, "terminal PAUSE_UPDATE for unknown flow %llu", m->flow_id);
        m->rc_flow_index = i;
        m->rc_terminal_flow_before = ns->flows[i];
        terminal_flow& f = ns->flows[i];
        if (!f.source_complete) {
            debug_terminal_event(ns, m, lp);
            advance_source_flow(f, tw_now(lp));
            if (f.remaining_data <= 1e-6) {
                complete_source_flow(ns, f, lp);
                return;
            }
            f.paused = m->pause_asserted ? 1 : 0;
            const double new_rate = f.paused ? 0.0 : f.allowed_rate;
            if (std::fabs(new_rate - f.send_rate) > EPS) {
                f.send_rate = new_rate;
                send_source_data_rate(ns, f, new_rate, lp);
            }
            schedule_source_completion(f, lp);
        }
    } else if (m->event_type == FLOW_REGISTER) {
        if (i < 0) {
            i = create_terminal_flow(ns, m, tw_now(lp));
            m->rc_flow_created = 1;
        }
        m->rc_flow_index = i;
        m->rc_terminal_flow_before = ns->flows[i];
        terminal_flow& f = ns->flows[i];
        debug_terminal_event(ns, m, lp);
        advance_dest_flow(f, tw_now(lp));
        f.receive_rate = std::max(0.0, m->rate);
        maybe_mark_destination_complete(f, tw_now(lp));
    } else if (m->event_type == DATA_RATE_UPDATE) {
        if (i < 0)
            tw_error(TW_LOC, "terminal DATA_RATE_UPDATE for unknown flow %llu", m->flow_id);
        m->rc_flow_index = i;
        m->rc_terminal_flow_before = ns->flows[i];
        terminal_flow& f = ns->flows[i];
        debug_terminal_event(ns, m, lp);
        advance_dest_flow(f, tw_now(lp));
        f.receive_rate = std::max(0.0, m->rate);
        maybe_mark_destination_complete(f, tw_now(lp));
    } else if (m->event_type == FLOW_COMPLETE) {
        if (i < 0)
            tw_error(TW_LOC, "terminal FLOW_COMPLETE for unknown flow %llu", m->flow_id);
        m->rc_flow_index = i;
        m->rc_terminal_flow_before = ns->flows[i];
        terminal_flow& f = ns->flows[i];
        if (ns->terminal_id == f.source_terminal) {
            if (f.source_complete) {
                m->rc_noop = 1;
                return;
            }
            advance_source_flow(f, tw_now(lp));
            if (m->expected_time_ns >= 0.0 &&
                std::fabs((double)(m->expected_time_ns - f.predicted_complete_ns)) > 0.5)
                return;
            if (f.remaining_data > 1e-6)
                return;
            debug_terminal_event(ns, m, lp);
            complete_source_flow(ns, f, lp);
        } else if (ns->terminal_id == f.destination_terminal) {
            debug_terminal_event(ns, m, lp);
            advance_dest_flow(f, tw_now(lp));
            f.close_received = 1;
            maybe_mark_destination_complete(f, tw_now(lp));
        }
    } else {
        tw_error(TW_LOC, "terminal received unknown event %d", m->event_type);
    }
}

static void terminal_rev_event(terminal_state* ns, tw_bf*, rate_msg* m, tw_lp*) {
    if (m->rc_noop)
        return;
    if (m->rc_terminal_multi) {
        for (int k = 0; k < ns->flow_count; ++k) {
            ns->flows[k].remaining_data = m->rc_terminal_remaining[k];
            ns->flows[k].send_rate = m->rc_terminal_send_rate[k];
            ns->flows[k].predicted_complete_ns = m->rc_terminal_predicted_complete[k];
            ns->flows[k].last_source_update_ns = m->rc_terminal_last_source_update[k];
            ns->flows[k].paused = m->rc_terminal_paused[k];
        }
        return;
    }
    if (m->rc_flow_created) {
        ns->flow_count = m->rc_prev_flow_count;
        if (ns->flow_count < MAX_FLOWS_PER_LP)
            std::memset(&ns->flows[ns->flow_count], 0, sizeof(ns->flows[ns->flow_count]));
    } else if (m->rc_flow_index >= 0) {
        ns->flows[m->rc_flow_index] = m->rc_terminal_flow_before;
    }
}

static const flow_spec* find_flow_spec(unsigned long long id) {
    for (const auto& f : flow_specs)
        if (f.flow_id == id)
            return &f;
    return nullptr;
}

static std::string rank_summary_path(int rank) {
    std::ostringstream os;
    os << cfg.flow_summary_file << ".rank" << rank;
    return os.str();
}

static void terminal_finalize(terminal_state* ns, tw_lp*) {
    std::ofstream out(rank_summary_path(mpi_rank_global), std::ios::app);
    if (!out)
        return;
    for (int i = 0; i < ns->flow_count; ++i) {
        const terminal_flow& f = ns->flows[i];
        if (!f.valid || f.destination_terminal != ns->terminal_id)
            continue;
        const flow_spec* spec = find_flow_spec(f.flow_id);
        if (!spec)
            continue;
        const double finish_seconds = f.finish_ns >= 0.0 ? ns_to_seconds(f.finish_ns) : -1.0;
        const double fct_seconds =
            finish_seconds >= 0.0 ? finish_seconds - spec->start_seconds : -1.0;
        out << f.flow_id << ',' << f.source_terminal << ',' << f.destination_terminal << ','
            << seconds_to_output_time(spec->start_seconds) << ',' << spec->size_data << ','
            << (f.destination_complete ? 1 : 0) << ',' << f.received_data << ',';
        if (finish_seconds >= 0.0)
            out << seconds_to_output_time(finish_seconds) << ','
                << seconds_to_output_time(fct_seconds);
        else
            out << "NA,NA";
        out << '\n';
    }
}

static tw_lptype terminal_lp = {(init_f)terminal_init,
                                nullptr,
                                (event_f)terminal_event,
                                (revent_f)terminal_rev_event,
                                nullptr,
                                (final_f)terminal_finalize,
                                (map_f)codes_mapping,
                                sizeof(terminal_state)};
static tw_lptype switch_lp = {(init_f)switch_init,
                              nullptr,
                              (event_f)switch_event,
                              (revent_f)switch_rev_event,
                              nullptr,
                              (final_f)switch_finalize,
                              (map_f)codes_mapping,
                              sizeof(switch_state)};

static void add_lp_types() {
    lp_type_register(TERMINAL_LP_NAME, &terminal_lp);
    lp_type_register(SWITCH_LP_NAME, &switch_lp);
}


static int configured_message_size() {
    int n = 0;
    if (configuration_get_value_int(&config, "PARAMS", "message_size", nullptr, &n) != 0)
        return 0;
    return n;
}

static void validate_message_size(int rank) {
    int n = configured_message_size();
    if (n <= 0 || (size_t)n < sizeof(rate_msg)) {
        if (rank == 0)
            std::fprintf(stderr,
                         "rate-fluid-wan: PARAMS.message_size=%d but sizeof(rate_msg)=%zu; "
                         "increase message_size\n",
                         n, sizeof(rate_msg));
        MPI_Abort(MPI_COMM_CODES, 1);
    }
}

static const char* find_config_arg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0)
            continue;

        /*
         * tw_init() consumes ROSS options and may also remove the "--"
         * separator. At this point the application configuration file is
         * therefore simply the remaining non-option argument. Accept both
         * layouts so this works whether ROSS preserves the separator or not.
         */
        if (argv[i][0] != '-')
            return argv[i];
    }
    return nullptr;
}

static void prepare_rank_summary_file() {
    std::ofstream out(rank_summary_path(mpi_rank_global), std::ios::trunc);
}

static void merge_summary_files() {
    MPI_Barrier(MPI_COMM_CODES);
    if (mpi_rank_global != 0)
        return;
    std::ofstream out(cfg.flow_summary_file, std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "could not write flow summary %s\n", cfg.flow_summary_file);
        return;
    }
    out << "flow_id,source_terminal,destination_terminal,start_" << time_unit_name() << ",size_"
        << data_unit_name() << ",completed,received_" << data_unit_name() << ",finish_"
        << time_unit_name() << ",fct_" << time_unit_name() << "\n";
    for (int rank = 0; rank < mpi_size_global; ++rank) {
        std::ifstream in(rank_summary_path(rank));
        std::string line;
        while (std::getline(in, line))
            if (!line.empty())
                out << line << '\n';
        std::remove(rank_summary_path(rank).c_str());
    }
}

int main(int argc, char** argv) {
    int rank = 0;

    tw_init(&argc, &argv);
    codes_comm_update();

    const char* config_file = find_config_arg(argc, argv);
    if (!config_file) {
        std::printf("Usage: mpirun -np <n> %s --sync=1 -- <config.{conf,yaml}>\n", argv[0]);
        MPI_Finalize();
        return 1;
    }

    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    MPI_Comm_size(MPI_COMM_CODES, &mpi_size_global);
    mpi_rank_global = rank;

    if (configuration_load(config_file, MPI_COMM_CODES, &config) != 0) {
        if (rank == 0)
            std::fprintf(stderr, "rate-fluid-wan: failed to load configuration %s\n", config_file);
        MPI_Finalize();
        return 1;
    }

    load_config();
    validate_message_size(rank);
    add_lp_types();
    codes_mapping_setup();

    /* Honor --end and the current CODES simulation.end_time/PARAMS.end_time
     * handling.  Use one year only as a fallback when neither was supplied. */
    if (g_tw_ts_end == 100000.0)
        g_tw_ts_end = seconds_to_ns(60.0 * 60.0 * 24.0 * 365.0);
    total_switch_lps = codes_mapping_get_lp_count(GROUP_NAME, 0, SWITCH_LP_NAME, nullptr, 1);
    total_terminal_lps = codes_mapping_get_lp_count(GROUP_NAME, 0, TERMINAL_LP_NAME, nullptr, 1);
    if (total_switch_lps != (int)switches.size())
        tw_error(TW_LOC, "config has %d switch LPs; topology has %zu", total_switch_lps,
                 switches.size());
    if (total_terminal_lps != (int)terminals.size())
        tw_error(TW_LOC, "config has %d terminal LPs; topology has %zu", total_terminal_lps,
                 terminals.size());
    if (rank == 0) {
        std::printf("rate-fluid-wan: switches=%zu terminals=%zu flows=%zu latency=%.6f ms "
                    "rate_unit=%s data_unit=%s time_unit=%s pause=%d debug=%d\n",
                    switches.size(), terminals.size(), flow_specs.size(),
                    cfg.link_latency_seconds * 1000.0, rate_unit_name(), data_unit_name(),
                    time_unit_name(), cfg.pause_enabled, cfg.debug);
    }
    prepare_rank_summary_file();
    MPI_Barrier(MPI_COMM_CODES);
    tw_run();
    merge_summary_files();
    configuration_free(&config);
    tw_end();
    return 0;
}
