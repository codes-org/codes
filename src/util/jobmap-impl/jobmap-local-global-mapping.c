/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "src/util/codes-jobmap-method-impl.h"


struct workload_params {
    int num_jobs;
    int *num_rank_job;
    int **lp_arrays;
};

static int jobmap_dumpi_configure(void const * params, void ** ctx)
{
    struct codes_jobmap_params_dumpi const * p = params;
    struct workload_params *wp = malloc(sizeof(*wp));
    assert(wp);

    /*open first time, count No. lines, each line is the LP list for a job*/
    FILE *alloc_file_name = fopen(p->alloc_file, "r");
    if(!alloc_file_name)
    {
        printf("Coudld not open file %s ======\n ", p->alloc_file);
        exit(1);
    }
    else{
        wp->num_jobs = 0;
        while(!feof(alloc_file_name))
        {
            char ch = (char)fgetc(alloc_file_name);
            if(ch == '\n')
                wp->num_jobs++;//how many jobs
        }
        fclose(alloc_file_name);
    }


    wp->num_rank_job = malloc(sizeof(wp->num_rank_job)*wp->num_jobs);
    assert(wp->num_rank_job);
    for(int i=0; i<wp->num_jobs; i++)
        wp->num_rank_job[i]=0;

    /*open second time, read No. of LP id in each line, No. of LP id equals to No. of rank in each job*/
    alloc_file_name = fopen(p->alloc_file, "r");
    int job_id = 0;
    while(!feof(alloc_file_name))
    {
        char ch = (char)fgetc(alloc_file_name);
        if(ch == '\n'){
            job_id++;//how many jobs
            continue;
        }
        if(ch == ' '){
            wp->num_rank_job[job_id]++;//how many ranks in each job
        }
    }
    fclose(alloc_file_name);

    /*open third time, read in each LP id for each rank in every job*/
    wp->lp_arrays = (int **)malloc(sizeof(int *)*wp->num_jobs);
    for(int i=0; i<wp->num_jobs; i++){
        wp->lp_arrays[i] = (int *)malloc(sizeof(int)*wp->num_rank_job[i]);
    }

    alloc_file_name = fopen(p->alloc_file, "r");
    for(int i=0; i < wp->num_jobs; i++)
    {
        for(int j=0; j < wp->num_rank_job[i]; j++)
        {
            fscanf(alloc_file_name, "%d", &wp->lp_arrays[i][j]);
        }
    }

    fclose(alloc_file_name);
    *ctx = wp;

    printf("There are %d Jobs\n", wp->num_jobs);
    for(int i=0; i < wp->num_jobs; i++)
    {
        printf("\nIn Job %d, there are %d ranks, LP list is:\n", i, wp->num_rank_job[i]);
        for(int j=0; j < wp->num_rank_job[i]; j++)
        {
            printf(",%d,", wp->lp_arrays[i][j]);
        }
        printf("\n==========\n");
    }

    return 0;
}

static struct codes_jobmap_id jobmap_dumpi_to_local(int id, void const * ctx)
{
    struct codes_jobmap_id rtn;
    struct workload_params *wp = (struct workload_params*)ctx;

    for(int i=0; i<wp->num_jobs; i++)
    {
        for(int j=0; j < wp->num_rank_job[i]; j++)
        {
            if(id == wp->lp_arrays[i][j])
            {
                rtn.job = i;
                rtn.rank = j;
                return rtn;
            }
            else{
                rtn.job = -1;
                rtn.rank = -1;
            }
        }
    }

    return rtn;
}

static int jobmap_dumpi_to_global(struct codes_jobmap_id id, void const * ctx)
{

    struct workload_params *wp = (struct workload_params*)ctx;

    if (id.job < wp->num_jobs)
        return wp->lp_arrays[id.job][id.rank];
    else
        return -1;
}

int jobmap_dumpi_get_num_jobs(void const * ctx)
{
    struct workload_params *wp = (struct workload_params*)ctx;
    return wp->num_jobs;

}


static void jobmap_dumpi_destroy(void * ctx)
{
    free(ctx);
}


struct codes_jobmap_impl jobmap_dumpi_impl = {
    jobmap_dumpi_configure,
    jobmap_dumpi_destroy,
    jobmap_dumpi_to_local,
    jobmap_dumpi_to_global,
    jobmap_dumpi_get_num_jobs
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
