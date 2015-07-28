/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "../codes-jobmap-method-impl.h"

#define LIST_DEBUG 0
#define dprintf(_fmt, ...) \
    do { \
        if (LIST_DEBUG) { \
            fprintf(stdout, "jobmap-list: "); \
            fprintf(stdout, _fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define ERR(str, ...)\
    do{\
        if (LIST_DEBUG) { \
            fprintf(stderr, "ERROR at %s:%d: " str "\n", \
                    __FILE__, __LINE__, ##__VA_ARGS__);\
        } \
        return -1; \
    }while(0)

struct jobmap_list {
    int num_jobs;
    int *num_rank_job;
    int **lp_arrays;
};

static int jobmap_list_configure(void const * params, void ** ctx)
{
    struct codes_jobmap_params_list const * p = params;
    struct jobmap_list *lst = malloc(sizeof(*lst));
    assert(lst);

    FILE *alloc_file_name = fopen(p->alloc_file, "r");
    if(!alloc_file_name) {
        ERR("Could not open file %s\n ", p->alloc_file);
    }
    else{
        lst->num_jobs = 0;
        while(!feof(alloc_file_name)) {
            char ch = (char)fgetc(alloc_file_name);
            if(ch == '\n')
                lst->num_jobs++;//how many jobs
        }
    }

    lst->num_rank_job = malloc(sizeof(lst->num_rank_job)*lst->num_jobs);
    assert(lst->num_rank_job);
    for(int i=0; i<lst->num_jobs; i++)
        lst->num_rank_job[i]=0;

    rewind(alloc_file_name);
    int job_id = 0;
    while(!feof(alloc_file_name)) {
        char ch = (char)fgetc(alloc_file_name);
        if(ch == '\n'){
            job_id++;//how many jobs
            continue;
        }
        if(ch == ' '){
            lst->num_rank_job[job_id]++;//how many ranks in each job
        }
    }

    lst->lp_arrays = (int **)malloc(sizeof(int *)*lst->num_jobs);
    for(int i=0; i<lst->num_jobs; i++){
        lst->lp_arrays[i] = (int *)malloc(sizeof(int)*lst->num_rank_job[i]);
    }

    rewind(alloc_file_name);
    for(int i=0; i < lst->num_jobs; i++) {
        for(int j=0; j < lst->num_rank_job[i]; j++) {
            fscanf(alloc_file_name, "%d", &lst->lp_arrays[i][j]);
        }
    }

    fclose(alloc_file_name);
    *ctx = lst;

    dprintf("read %d jobs\n", lst->num_jobs);
    for(int i=0; i < lst->num_jobs; i++) {
        dprintf(" job %d contains %d ranks, LP list is:\n   ",
                i, lst->num_rank_job[i]);
        for(int j=0; j < lst->num_rank_job[i]; j++) {
            dprintf(" %d,", lst->lp_arrays[i][j]);
        }
        dprintf("\n==========\n");
    }

    return 0;
}

static struct codes_jobmap_id jobmap_list_to_local(int id, void const * ctx)
{
    struct codes_jobmap_id rtn;
    rtn.job = -1;
    rtn.rank = -1;

    struct jobmap_list *lst = (struct jobmap_list*)ctx;

    for(int i=0; i<lst->num_jobs; i++) {
        for(int j=0; j < lst->num_rank_job[i]; j++) {
            if(id == lst->lp_arrays[i][j]) {
                rtn.job = i;
                rtn.rank = j;
                return rtn;
            }
        }
    }

    return rtn;
}

static int jobmap_list_to_global(struct codes_jobmap_id id, void const * ctx)
{

    struct jobmap_list *lst = (struct jobmap_list*)ctx;

    if (id.job < lst->num_jobs)
        return lst->lp_arrays[id.job][id.rank];
    else
        return -1;
}

int jobmap_list_get_num_jobs(void const * ctx)
{
    struct jobmap_list *lst = (struct jobmap_list*)ctx;
    return lst->num_jobs;

}


static void jobmap_list_destroy(void * ctx)
{
    struct jobmap_list *lst = (struct jobmap_list*)ctx;
    for(int i=0; i<lst->num_jobs; i++){
        free(lst->lp_arrays[i]);
    }

    free(lst->lp_arrays);
    free(lst->num_rank_job);
    free(ctx);
}


struct codes_jobmap_impl jobmap_list_impl = {
    jobmap_list_configure,
    jobmap_list_destroy,
    jobmap_list_to_local,
    jobmap_list_to_global,
    jobmap_list_get_num_jobs
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
