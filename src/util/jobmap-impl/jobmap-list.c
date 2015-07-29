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

#define COND_REALLOC(_len_expr, _cap_var, _buf_var) \
    do { \
        if ((_len_expr) == _cap_var) { \
            _buf_var = realloc(_buf_var, (_cap_var)*2*sizeof(*(_buf_var))); \
            assert(_buf_var); \
            _cap_var *= 2; \
        } \
    } while (0)

static int parse_line(
        FILE *f,
        char **line_buf,
        int *line_cap,
        int *num_ranks,
        int **rank_list)
{
    char * buf = *line_buf;

    int cap = *line_cap;
    int pos = 0;

    *num_ranks = 0;

    for (int c = fgetc(f); c != EOF && c != '\n'; c = fgetc(f)) {
        buf[pos++] = (char)c;
        COND_REALLOC(pos, cap, buf);
    }
    if (ferror(f)) {
        *num_ranks = -1;
        goto end;
    }
    else {
        buf[pos]='\0';
    }

    int list_cap = 8;
    int *lst = malloc(list_cap * sizeof(*lst));
    assert(lst);
    int rank;

    *num_ranks = 0;
    for (char * tok = strtok(buf, " \t\r"); tok != NULL;
            tok = strtok(NULL, " \t\r")) {
        int rc = sscanf(tok, "%d", &rank);
        if (rc != 1) {
            fprintf(stderr,
                    "jobmap-list: unable to read alloc file - bad rank (%s)\n",
                    tok);
            *num_ranks = -1;
            break;
        }
        COND_REALLOC(*num_ranks, list_cap, lst);
        lst[*num_ranks] = rank;
        *num_ranks += 1;
    }

    if (*num_ranks <= 0) {
        *rank_list = NULL;
        free(lst);
    }
    else
        *rank_list = lst;
end:
    *line_buf = buf;
    *line_cap = cap;
    return (*num_ranks < 0) ? -1 : 0;
}

static int jobmap_list_configure(void const * params, void ** ctx)
{
    struct codes_jobmap_params_list const * p = params;
    struct jobmap_list *lst = malloc(sizeof(*lst));
    assert(lst);

    FILE *f = fopen(p->alloc_file, "r");
    if(!f) {
        ERR("Could not open file %s", p->alloc_file);
    }

    // job storage
    lst->num_jobs = 0;
    int job_cap = 8;
    lst->num_rank_job = calloc(job_cap, sizeof(*lst->num_rank_job));
    assert(lst->num_rank_job);
    lst->lp_arrays = calloc(job_cap, sizeof(*lst->lp_arrays));
    assert(lst->lp_arrays);

    // line storage
    int line_cap = 1<<10;
    char *line_buf = malloc(line_cap);
    assert(line_buf);

    int rc = 0;
    do {
        rc = parse_line(f, &line_buf, &line_cap,
                &lst->num_rank_job[lst->num_jobs],
                &lst->lp_arrays[lst->num_jobs]);
        if (rc == -1) {
            // error and exit
            if (ferror(f)) {
                perror("fgets");
                break;
            }
        }
        else if (lst->num_rank_job[lst->num_jobs] > 0) {
            lst->num_jobs++;
        }
        // resize if needed
        if (!feof(f) && lst->num_jobs == job_cap) {
            int tmp = job_cap;
            COND_REALLOC(lst->num_jobs, tmp, lst->num_rank_job);
            COND_REALLOC(lst->num_jobs, job_cap, lst->lp_arrays);
        }
    } while (!feof(f));

    if (rc == 0) {
        fclose(f);
        free(line_buf);
        *ctx = lst;
        return 0;
    }
    else {
        for (int i = 0; i < job_cap; i++) {
            free(lst->lp_arrays[i]);
        }
        free(lst->lp_arrays);
        free(lst->num_rank_job);
        free(lst);
        *ctx = NULL;
        return -1;
    }
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

static int jobmap_list_get_num_jobs(void const * ctx)
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
