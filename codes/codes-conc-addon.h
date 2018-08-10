/*
 * Copyright (C) 2017 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef CODES_CONC_ADDON_H
#define CODES_CONC_ADDON_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USE_CONC
#include <ncptl/ncptl.h> 
#endif

#define MAX_CONC_ARGC 20

typedef struct conc_bench_param conc_bench_param;

/* implementation structure */
struct codes_conceptual_bench {
    char *program_name; /* name of the conceptual program */
    int (*conceptual_main)(int* argc, char *argv[]);
};

struct conc_bench_param {
    char *conc_program;
    int conc_argc;
    char *conc_argv[MAX_CONC_ARGC];
};


int codes_conc_bench_load(
        const char* program,
        int* argc, 
        const char *argv[]);


void codes_conceptual_add_bench(struct codes_conceptual_bench const * method);


#ifdef __cplusplus
}
#endif

#endif /* CODES_CONC_ADDON_H */
