#include "codes/codes-workload-config.h"

/* ross.h must precede configuration.h so <mpi.h> is first pulled in outside an
 * extern "C" context (ross.h does this deliberately); configuration.h includes
 * <mpi.h> inside its own extern "C" block, which trips MPICH's C++ bindings when
 * this file is compiled as C++. */
#include <ross.h>

#include "codes/configuration.h"

#include <cstring>

/* the section the YAML front-end emits synthetic workload params into. */
#define CODES_WORKLOAD_SECTION "WORKLOAD"

/* the section the compiler emits for an explicit multi-job placement; its
 * num_jobs marker key is how a reader detects the multi-job form. */
#define CODES_JOBS_SECTION "JOBS"

void codes_workload_config_check_unsupported_jobs(const char* model_name) {
    int num_jobs;
    if (configuration_get_value_int(&config, CODES_JOBS_SECTION, "num_jobs", NULL, &num_jobs) != 0)
        return; /* no JOBS section: single-workload or legacy config */
    tw_error(TW_LOC,
             "config error: this config places %d jobs (a multi-job placement), but %s runs a "
             "single global synthetic workload and cannot execute per-job placement yet; use a "
             "single workload: (or a one-job jobs: entry on all nodes), or model-net-mpi-replay "
             "for multi-job runs",
             num_jobs, model_name);
}

void codes_workload_config_apply_int(const char* key, int* val, int cli_default) {
    /* a value other than the registered default means the command line set it. */
    if (*val != cli_default)
        return;
    int v;
    if (configuration_get_value_int(&config, CODES_WORKLOAD_SECTION, key, NULL, &v) == 0)
        *val = v;
}

void codes_workload_config_apply_double(const char* key, double* val, double cli_default) {
    if (*val != cli_default)
        return;
    double v;
    if (configuration_get_value_double(&config, CODES_WORKLOAD_SECTION, key, NULL, &v) == 0)
        *val = v;
}

void codes_workload_config_apply_traffic(int* val, int cli_default,
                                         const struct codes_workload_traffic_name* names) {
    if (*val != cli_default)
        return;
    char buf[CONFIGURATION_MAX_NAME];
    if (configuration_get_value(&config, CODES_WORKLOAD_SECTION, "traffic", NULL, buf,
                                sizeof(buf)) <= 0)
        return;
    for (const struct codes_workload_traffic_name* n = names; n->name != NULL; ++n) {
        if (std::strcmp(n->name, buf) == 0) {
            *val = n->value;
            return;
        }
    }
    tw_error(TW_LOC, "config error: workload traffic pattern \"%s\" is not supported by this model",
             buf);
}
