/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <assert.h>
#include <ross.h>
#include <codes/codes.h>
#include <codes/codes-conc-addon.h>


/* list of available benchmarks begin */
extern struct codes_conceptual_bench bisect_bench;
extern struct codes_conceptual_bench cosmoflow_bench;
extern struct codes_conceptual_bench hotpotato_bench;
extern struct codes_conceptual_bench latencyall_bench;
extern struct codes_conceptual_bench latency_bench;
/* list of available benchmarks end */

static struct codes_conceptual_bench const * bench_array_default[] =
{
    /* default benchmarks begin */
    &bisect_bench,
    &cosmoflow_bench,
    &hotpotato_bench,
    &latencyall_bench,
    &latency_bench,
    /* default benchmarks end */
    NULL
};

// once initialized, adding a bench generator is an error
static int is_bench_init = 0;
static int num_user_benchs = 0;
static struct codes_conceptual_bench const ** bench_array = NULL;



// only call this once
static void init_bench_methods(void)
{
    if (is_bench_init)
        return;
    if (bench_array == NULL)
        bench_array = bench_array_default;
    else {
        // note - includes null char
        int num_default_benchs =
            (sizeof(bench_array_default) / sizeof(bench_array_default[0]));
        printf("\n Num default methods %d ", num_default_benchs);
        bench_array = realloc(bench_array,
                (num_default_benchs + num_user_benchs + 1) *
                sizeof(*bench_array));
        memcpy(bench_array+num_user_benchs, bench_array_default,
                num_default_benchs * sizeof(*bench_array_default));
    }
    is_bench_init = 1;
}


int codes_conc_bench_load(
        const char *program,
        int argc, 
        char *argv[])
{
    init_bench_methods();

    int i;
    int ret;

    for(i=0; bench_array[i] != NULL; i++)
    {
        if(strcmp(bench_array[i]->program_name, program) == 0)
        {
            /* load appropriate workload generator */
            ret = bench_array[i]->conceptual_main(argc, argv);
            if(ret < 0)
            {
                return(-1);
            }
            return(i);
        }
    }
    fprintf(stderr, "Error: failed to find benchmark program %s\n", program);
    return(-1);
}

void codes_conceptual_add_bench(struct codes_conceptual_bench const * bench)
{
    static int bench_array_cap = 10;
    if (is_bench_init)
        tw_error(TW_LOC,
                "adding a conceptual benchmark method after initialization is forbidden");
    else if (bench_array == NULL){
        bench_array = malloc(bench_array_cap * sizeof(*bench_array));
        assert(bench_array);
    }

    if (num_user_benchs == bench_array_cap) {
        bench_array_cap *= 2;
        bench_array = realloc(bench_array,
                bench_array_cap * sizeof(*bench_array));
        assert(bench_array);
    }
    bench_array[num_user_benchs++] = bench;
}



