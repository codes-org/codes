#include <codes/surrogate/init.h>
#include <codes/surrogate/switch.h>
#include <codes/surrogate/packet-latency-predictor/average.h>
#include <codes/surrogate/packet-latency-predictor/torch-jit.h>

bool freeze_network_on_switch = true;
struct surrogate_config surr_config = {0};
bool is_surrogate_configured = false;
double surrogate_switching_time = 0.0;

struct switch_at_struct switch_at;


// === Stats!
void print_surrogate_stats(void) {
    if(is_surrogate_configured && g_tw_mynode == 0) {
        printf("\nTotal time spent on switching from and to surrogate-mode: %.4f\n", (double) surrogate_switching_time / g_tw_clock_rate);
    }
}
// === END OF Stats!


// === All things Surrogate Configuration
void surrogate_configure(
        char const * const anno,
        struct surrogate_config * const sc,
        struct packet_latency_predictor ** pl_pred
) {
    assert(sc);
    assert(0 < sc->n_lp_types && sc->n_lp_types <= MAX_LP_TYPES);
    is_surrogate_configured = true;

    // This is the only place where the director data should be loaded and set up
    surr_config = *sc;

    // Determining which director mode to set up
    char director_mode[MAX_NAME_LENGTH];
    director_mode[0] = '\0';
    configuration_get_value(&config, "SURROGATE", "director_mode", anno, director_mode, MAX_NAME_LENGTH);
    if (strcmp(director_mode, "at-fixed-virtual-times") == 0) {
        PRINTF_ONCE("\nSurrogate activated switching at fixed virtual times: ");

        // Loading timestamps
        char **timestamps;
        size_t len;
        configuration_get_multivalue(&config, "SURROGATE", "fixed_switch_timestamps", anno, &timestamps, &len);

        switch_at.current_i = 0;
        switch_at.total = len;
        switch_at.time_stampts = malloc(len * sizeof(double));

        for (size_t i = 0; i < len; i++) {
            errno = 0;
            switch_at.time_stampts[i] = strtod(timestamps[i], NULL);
            if (errno == ERANGE || errno == EILSEQ){
                tw_error(TW_LOC, "Sequence `%s' could not be succesfully interpreted as a _double_.", timestamps[i]);
            }

            PRINTF_ONCE("%g%s", switch_at.time_stampts[i], i == len-1 ? "" : ", ");
        }
        PRINTF_ONCE("\n");

        // Injecting into ROSS the function to be called at GVT and the instant in time to trigger GVT
        g_tw_gvt_arbitrary_fun = director_switch;

#ifdef USE_RAND_TIEBREAKER
        tw_event_sig time_stamp = {0};
        time_stamp.recv_ts = switch_at.time_stampts[0];
        tw_trigger_arbitrary_fun_at(time_stamp);
#else
        tw_trigger_arbitrary_fun_at(switch_at.time_stampts[0]);
#endif

        // freeing timestamps before it dissapears
        for (size_t i = 0; i < len; i++) {
            free(timestamps[i]);
        }
        free(timestamps);
    } else {
        tw_error(TW_LOC, "Unknown director mode `%s`", director_mode);
    }

    // Determining which predictor to set up and return
    char latency_pred_name[MAX_NAME_LENGTH];
    latency_pred_name[0] = '\0';
    configuration_get_value(&config, "SURROGATE", "packet_latency_predictor", anno, latency_pred_name, MAX_NAME_LENGTH);
    if (*latency_pred_name) {
        if (strcmp(latency_pred_name, "average") == 0) {
            *pl_pred = &average_latency_predictor;

            // Finding out whether to ignore some packet latencies
            int rc = configuration_get_value_double(&config, "SURROGATE", "ignore_until", anno, &ignore_until);
            if (rc) {
                ignore_until = -1; // any negative number disables ignore_until, all packet latencies will be considered
                PRINTF_ONCE("Enabling average packet latency predictor\n");
            } else {
                PRINTF_ONCE("Enabling average packet latency predictor with ignore_until=%g\n", ignore_until);
            }
        } else if (strcmp(latency_pred_name, "torch-jit") == 0) {
            char torch_jit_mode[MAX_NAME_LENGTH];
            torch_jit_mode[0] = '\0';
            configuration_get_value(&config, "SURROGATE", "torch_jit_mode", anno, torch_jit_mode, MAX_NAME_LENGTH);
            if (strcmp(torch_jit_mode, "single-static-model-for-all-terminals") != 0) {
                tw_error(TW_LOC, "Unknown torch-jit mode `%s`", torch_jit_mode);
            }

            char torch_jit_model_path[MAX_NAME_LENGTH];
            torch_jit_model_path[0] = '\0';
            configuration_get_value(&config, "SURROGATE", "torch_jit_model_path", anno, torch_jit_model_path, MAX_NAME_LENGTH);
            surrogate_torch_init(torch_jit_model_path);

            *pl_pred = &torch_latency_predictor;
        } else {
            tw_error(TW_LOC, "Unknown predictor for packet latency `%s` "
                    "(possibilities include: average, torch-jit)", latency_pred_name);
        }
    } else {
        *pl_pred = &average_latency_predictor;
        PRINTF_ONCE("Enabling average packet latency predictor (default behaviour)\n");
    }

    // Determining which predictor to set up and return
    char network_treatment_name[MAX_NAME_LENGTH];
    network_treatment_name[0] = '\0';
    configuration_get_value(&config, "SURROGATE", "network_treatment_on_switch", anno, network_treatment_name, MAX_NAME_LENGTH);
    if (*network_treatment_name) {
        if (strcmp(network_treatment_name, "freeze") == 0) {
            freeze_network_on_switch = true;
            PRINTF_ONCE("The network will be frozen on switch to surrogate\n");
        } else if (strcmp(network_treatment_name, "nothing") == 0) {
            freeze_network_on_switch = false;
            PRINTF_ONCE("The network will be left alone on switch to surrogate (it will run on the background until it empties by itself)\n");
        } else {
            tw_error(TW_LOC, "Unknown network treatment `%s` (possibilities include: frezee or nothing)", network_treatment_name);
        }
    } else {
        freeze_network_on_switch = true;
        PRINTF_ONCE("The network will be frozen on switch to surrogate (default behaviour)\n");
    }

    //surr_config.director.switch_surrogate();
    if (DEBUG_DIRECTOR && g_tw_mynode == 0) {
        fprintf(stderr, "Simulation starting on %s mode\n", surr_config.director.is_surrogate_on() ? "surrogate" : "vanilla");
    }
}
// === END OF All things Surrogate Configuration
