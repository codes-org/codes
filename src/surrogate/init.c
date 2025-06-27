#include <codes/surrogate/init.h>
#include <codes/surrogate/packet-latency-predictor/average.h>
#include <codes/surrogate/application-surrogate.h>
#include <codes/surrogate/network-surrogate.h>
#include <codes/surrogate/app-iteration-predictor/average.h>

#ifdef USE_TORCH
#include <codes/surrogate/packet-latency-predictor/torch-jit.h>
#endif

#define master_printf(...) if (g_tw_mynode == 0) { printf(__VA_ARGS__); }

// Shared times across network and application surrogates
double surrogate_switching_time = 0.0;
double time_in_surrogate = 0.0;
double surrogate_time_last = 0.0;

static bool network_director_enabled = false;
static bool is_network_surrogate_configured = false;
static bool is_app_surrogate_configured = false;
static struct packet_latency_predictor current_net_predictor = {0};
static struct app_iteration_predictor current_iter_predictor = {0};


// === Stats!
void print_surrogate_stats(void) {
    // Computing the time in surrogate only makes sense if we can switch the whole simulation all at once (like the network simulation does), and it doesn't work with the application surrogate as this doesn't switch the state of the simulation all at once
    if(is_network_surrogate_configured && g_tw_mynode == 0) {
        printf("\nTotal time spent on surrogate-mode: %.4f\n", (double) time_in_surrogate / g_tw_clock_rate);
        printf("Total time spent on switching from and to surrogate-mode: %.4f\n", (double) surrogate_switching_time / g_tw_clock_rate);
    }
}
// === END OF Stats!


// === All things Surrogate Configuration
bool network_surrogate_configure(
        char const * const anno,
        struct network_surrogate_config * const sc,
        struct packet_latency_predictor ** pl_pred
) {
    assert(sc);
    assert(0 < sc->n_lp_types && sc->n_lp_types <= MAX_LP_TYPES);
    is_network_surrogate_configured = true;

    struct switch_at_struct switch_network_at;

    // Determining which director mode to set up
    char director_mode[MAX_NAME_LENGTH];
    director_mode[0] = '\0';
    configuration_get_value(&config, "NETWORK_SURROGATE", "director_mode", anno, director_mode, MAX_NAME_LENGTH);
    if (strcmp(director_mode, "at-fixed-virtual-times") == 0) {
        master_printf("\nNetwork surrogate activated switching at fixed virtual times: ");

        // Loading timestamps
        char **timestamps;
        size_t len;
        configuration_get_multivalue(&config, "NETWORK_SURROGATE", "fixed_switch_timestamps", anno, &timestamps, &len);

        network_director_enabled = true;
        switch_network_at.current_i = 0;
        switch_network_at.total = len;
        switch_network_at.time_stampts = malloc(len * sizeof(double));

        for (size_t i = 0; i < len; i++) {
            errno = 0;
            switch_network_at.time_stampts[i] = strtod(timestamps[i], NULL);
            if (errno == ERANGE || errno == EILSEQ){
                tw_error(TW_LOC, "Sequence `%s' could not be succesfully interpreted as a _double_.", timestamps[i]);
            }

            master_printf("%g%s", switch_network_at.time_stampts[i], i == len-1 ? "" : ", ");
        }
        master_printf("\n");

        // freeing timestamps before it dissapears
        for (size_t i = 0; i < len; i++) {
            free(timestamps[i]);
        }
        free(timestamps);
    } else if (strcmp(director_mode, "delegate-to-app-director") == 0) {
        master_printf("\nNetwork surrogate enabled but director won't run. Network surrogate will be triggered by app director if present\n");
    } else {
        tw_error(TW_LOC, "Unknown director mode `%s`", director_mode);
    }

    // Determining which predictor to set up and return
    char latency_pred_name[MAX_NAME_LENGTH];
    latency_pred_name[0] = '\0';
    configuration_get_value(&config, "NETWORK_SURROGATE", "packet_latency_predictor", anno, latency_pred_name, MAX_NAME_LENGTH);
    if (*latency_pred_name) {
        if (strcmp(latency_pred_name, "average") == 0) {
            current_net_predictor = average_latency_predictor(sc->total_terminals);
            *pl_pred = &current_net_predictor;

#ifdef USE_TORCH
        } else if (strcmp(latency_pred_name, "torch-jit") == 0) {
            char torch_jit_mode[MAX_NAME_LENGTH];
            torch_jit_mode[0] = '\0';
            configuration_get_value(&config, "NETWORK_SURROGATE", "torch_jit_mode", anno, torch_jit_mode, MAX_NAME_LENGTH);
            if (strcmp(torch_jit_mode, "single-static-model-for-all-terminals") != 0) {
                tw_error(TW_LOC, "Unknown torch-jit mode `%s`", torch_jit_mode);
            }

            char torch_jit_model_path[MAX_NAME_LENGTH];
            torch_jit_model_path[0] = '\0';
            configuration_get_value(&config, "NETWORK_SURROGATE", "torch_jit_model_path", anno, torch_jit_model_path, MAX_NAME_LENGTH);
            surrogate_torch_init(torch_jit_model_path);

            *pl_pred = &torch_latency_predictor;
#endif

        } else {
            tw_error(TW_LOC, "Unknown predictor for packet latency `%s` "
                    "(possibilities include: average"
#ifdef USE_TORCH
                    ", torch-jit"
#endif
                    ")", latency_pred_name);
        }
    } else {
        current_net_predictor = average_latency_predictor(sc->total_terminals);
        *pl_pred = &current_net_predictor;
        master_printf("Enabling average packet latency predictor (default behaviour)\n");
    }

    // Finding out whether to ignore some packet latencies
    int rc = configuration_get_value_double(&config, "NETWORK_SURROGATE", "ignore_until", anno, &ignore_until);
    if (rc) {
        ignore_until = -1; // any negative number disables ignore_until, all packet latencies will be considered
        master_printf("`ignore_until` disabled (all packet latencies will be used in training the predictor)\n");
    } else {
        master_printf("ignore_until=%g a packet delievered before this time stamp will not be used in training any predictor\n", ignore_until);
    }

    bool freeze_network_on_switch = true;
    // Determining which predictor to set up and return
    char network_treatment_name[MAX_NAME_LENGTH];
    network_treatment_name[0] = '\0';
    configuration_get_value(&config, "NETWORK_SURROGATE", "network_treatment_on_switch", anno, network_treatment_name, MAX_NAME_LENGTH);
    if (*network_treatment_name) {
        if (strcmp(network_treatment_name, "freeze") == 0) {
            freeze_network_on_switch = true;
            master_printf("The network will be frozen on switch to surrogate\n");
        } else if (strcmp(network_treatment_name, "nothing") == 0) {
            freeze_network_on_switch = false;
            master_printf("The network will be left alone on switch to surrogate (it will run on the background until it empties by itself)\n");
        } else {
            tw_error(TW_LOC, "Unknown network treatment `%s` (possibilities include: frezee or nothing)", network_treatment_name);
        }
    } else {
        freeze_network_on_switch = true;
        master_printf("The network will be frozen on switch to surrogate (default behaviour)\n");
    }

    network_director_configure(sc, network_director_enabled ? &switch_network_at: NULL, freeze_network_on_switch);

    //surr_config.director.switch_surrogate();
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        fprintf(stderr, "Simulation starting on network %s mode\n", sc->model.is_surrogate_on() ? "surrogate" : "high-fidelity");
    }

    return freeze_network_on_switch;
}

static int load_and_validate_int_param(const char* param_name, int default_value) {
    char param_str[MAX_NAME_LENGTH];
    param_str[0] = '\0';
    int const rc = configuration_get_value(&config, "APPLICATION_SURROGATE", param_name, NULL, param_str, MAX_NAME_LENGTH);
    int value = (rc > 0) ? atoi(param_str) : default_value;

    if (value <= 0) {
        tw_warning(TW_LOC, "%s must be a positive integer, got %d. Using default value %d.", param_name, value, default_value);
        value = default_value;
    }

    return value;
}

static double load_and_validate_double_param(const char* param_name, double default_value) {
    char param_str[MAX_NAME_LENGTH];
    param_str[0] = '\0';
    int const rc = configuration_get_value(&config, "APPLICATION_SURROGATE", param_name, NULL, param_str, MAX_NAME_LENGTH);
    double value = (rc > 0) ? strtod(param_str, NULL) : default_value;

    if (value <= 0) {
        tw_warning(TW_LOC, "%s must be a positive integer, got %d. Using default value %d.", param_name, value, default_value);
        value = default_value;
    }

    return value;
}

static struct application_director_config load_director_config(void) {
    int const default_gvt = 100;
    double const default_ns = 1.0e6; // 1ms

    enum {
        MODE_NOT_SET,
        MODE_EVERY_N_GVT,
        MODE_EVERY_N_NANOSECONDS,
        MODE_UNKNOWN
    } mode;

    char director_mode[MAX_NAME_LENGTH];
    director_mode[0] = '\0';
    int const rc_mode = configuration_get_value(&config, "APPLICATION_SURROGATE", "director_mode", NULL, director_mode, MAX_NAME_LENGTH);

    if (rc_mode == 0) {
        mode = MODE_NOT_SET;
    } else if (strcmp(director_mode, "every-n-gvt") == 0) {
        mode = MODE_EVERY_N_GVT;
    } else if (strcmp(director_mode, "every-n-nanoseconds") == 0) {
        mode = MODE_EVERY_N_NANOSECONDS;
    } else {
        mode = MODE_UNKNOWN;
    }

    int every_n_gvt = load_and_validate_int_param("director_num_gvt", default_gvt);
    double every_n_ns = load_and_validate_double_param("director_num_ns", default_ns);

    bool const is_sequential = (g_tw_synchronization_protocol == SEQUENTIAL ||
                                g_tw_synchronization_protocol == SEQUENTIAL_ROLLBACK_CHECK);

    struct application_director_config config;
    switch (mode) {
        case MODE_EVERY_N_GVT:
            if (is_sequential) {
                tw_warning(TW_LOC, "Cannot use 'every-n-gvt' mode in sequential simulation. Forcing 'every-n-nanoseconds' mode.");
                config.option = APP_DIRECTOR_OPTS_call_every_ns;
                config.call_every_ns = every_n_ns;
            } else {
                config.option = APP_DIRECTOR_OPTS_every_n_gvt;
                config.every_n_gvt = every_n_gvt;
            }
            break;

        case MODE_EVERY_N_NANOSECONDS:
            config.option = APP_DIRECTOR_OPTS_call_every_ns;
            config.call_every_ns = every_n_ns;
            break;

        case MODE_UNKNOWN:
            tw_warning(TW_LOC, "Unknown director_mode '%s'. Using default mode 'every-n-nanoseconds'.", director_mode);
            config.option = APP_DIRECTOR_OPTS_call_every_ns;
            config.call_every_ns = every_n_ns;
            break;

        case MODE_NOT_SET:
        default:
            tw_warning(TW_LOC, "director_mode not set. Using default mode 'every-n-nanoseconds'.");
            config.option = APP_DIRECTOR_OPTS_call_every_ns;
            config.call_every_ns = every_n_ns;
            break;
    }

    config.use_network_surrogate = is_network_surrogate_configured;

    return config;
}

void application_surrogate_configure(
    int num_terminals_in_pe,
    int num_apps,
    struct app_iteration_predictor ** iter_pred
) {
    char num_iters_str[MAX_NAME_LENGTH];
    num_iters_str[0] = '\0';
    int const rc = configuration_get_value(&config, "APPLICATION_SURROGATE", "num_iters_to_collect", NULL, num_iters_str, MAX_NAME_LENGTH);
    int const num_of_iters_to_feed = (rc > 0) ? atoi(num_iters_str) : 5; // default to 5 if not specified

    struct avg_app_config predictor_config = {
        .num_apps = num_apps,
        .num_nodes_in_pe = num_terminals_in_pe,
        .num_iters_to_collect = num_of_iters_to_feed,
    };

    struct application_director_config app_dir_config = load_director_config();

    current_iter_predictor = avg_app_iteration_predictor(&predictor_config);
    application_director_configure(&app_dir_config, &current_iter_predictor);
    *iter_pred = &current_iter_predictor;
    is_app_surrogate_configured = true;

    // Printing configuration summary
    master_printf("\nApplication surrogate configuration:\n");
    master_printf("  Predictor - num_apps: %d, num_iters_to_collect: %d\n",
                  predictor_config.num_apps, predictor_config.num_iters_to_collect);

    if (app_dir_config.option == APP_DIRECTOR_OPTS_every_n_gvt) {
        master_printf("  Director - mode: every-n-gvt, every_n_gvt: %d\n", app_dir_config.every_n_gvt);
    } else {
        master_printf("  Director - mode: every-n-nanoseconds, call_every_ns: %e\n", app_dir_config.call_every_ns);
    }
    if (network_director_enabled) {
        master_printf("  The network director has been replaced by the application director. The application director will trigger the network surrogate on and off.\n");
    }
    master_printf("\n");
}

void surrogates_finalize(void) {
    // TODO (helq): check that we are in fact still in surrogate (either network or application)
    if (surrogate_time_last > 0) { // we likely didn't transitioned back from surrogate mode
        time_in_surrogate += tw_clock_read() - surrogate_time_last;
    }
    if (is_network_surrogate_configured) {
        network_director_finalize();
    }
    if (is_app_surrogate_configured) {
        application_director_finalize();
        free_avg_app_iteration_predictor();
    }
}
// === END OF All things Surrogate Configuration
