/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <ross.h>
#include <codes/codes-workload.h>
#include <codes/quickhash.h>
#include <codes/quicklist.h>
#include <codes/codes.h>

#define DEFAULT_HASH_TBL_SIZE 1024

struct app_params
{
    iomock_params params;
    int app_id;
    struct qlist_head ql;
};

struct rank_state
{
    int rank;
    int app_id;
    int reqs_processed;
    int has_opened;
    int has_closed;
    struct app_params *params;
    struct qhash_head hash_link;
};

// globals
// hold all of the ranks using this workload
static struct qhash_table *rank_tbl = NULL;
static int num_qhash_entries = 0;

// hold application configurations
static struct qlist_head app_params_list = QLIST_HEAD_INIT(app_params_list);

// check parameter sets for equality
static int params_eq(iomock_params const * a, iomock_params const * b)
{
    return (a->file_id == b->file_id &&
            a->use_uniq_file_ids == b->use_uniq_file_ids &&
            a->is_write == b->is_write &&
            a->num_requests == b->num_requests &&
            a->request_size == b->request_size);
}

// comparison function for hash table
static int rank_tbl_compare(void * key, struct qhash_head * link)
{
    struct rank_state *a = key;
    struct rank_state *b = qhash_entry(link, struct rank_state, hash_link);
    if (a->rank == b->rank && a->app_id == b->app_id)
        return 1;
    else
        return 0;
}

// calculate file id depending on config
static uint64_t get_file_id(iomock_params const * p, int rank)
{
    return p->file_id + (p->use_uniq_file_ids ? rank : 0);
}

static void * iomock_workload_read_config(
        ConfigHandle * handle,
        char const * section_name,
        char const * annotation,
        int num_ranks)
{
    (void)num_ranks; // not needed for this workload
    iomock_params *p = malloc(sizeof(*p));
    assert(p);

    // defaults
    p->file_id = 0;
    p->is_write = 1;
    p->use_uniq_file_ids = 0;
    p->rank_table_size = 0;

    char req_type[CONFIGURATION_MAX_NAME] = "";

    int rc;

    // required
    rc = configuration_get_value_int(handle, section_name, "mock_num_requests",
            annotation, &p->num_requests);
    if (rc || p->num_requests < 0)
        tw_error(TW_LOC,
                "mock workload: expected non-negative integer for "
                "\"mock_num_requests\"");
    rc = configuration_get_value_int(handle, section_name, "mock_request_size",
            annotation, &p->request_size);
    if (rc || p->request_size < 0)
        tw_error(TW_LOC,
                "mock workload: expected positive integer for "
                "\"mock_request_size\"");

    // optionals
    rc = configuration_get_value(handle, section_name, "mock_request_type",
            annotation, req_type, CONFIGURATION_MAX_NAME);
    if (rc > 0) {
        if (strcmp(req_type, "write") == 0)
            p->is_write = 1;
        else if (strcmp(req_type, "read") == 0)
            p->is_write = 0;
        else
            tw_error(TW_LOC,
                    "mock workload: expected \"read\" or \"write\" for "
                    "mock_request_type, got %s\n", req_type);
    }

    long int file_id;
    rc = configuration_get_value_longint(handle, section_name, "mock_file_id",
            annotation, &file_id);
    if (!rc)
        p->file_id = file_id;

    configuration_get_value_int(handle, section_name,
            "mock_use_unique_file_ids", annotation, &p->use_uniq_file_ids);

    rc = configuration_get_value_int(handle, section_name,
            "mock_rank_table_size", annotation, &p->rank_table_size);
    if (!rc && p->rank_table_size < 0)
        tw_error(TW_LOC, "mock_rank_table_size expected to be positive\n");

    return p;
}

/* load the workload file */
static int iomock_workload_load(const void* params, int app_id, int rank)
{
    iomock_params const * p = (iomock_params const *) params;

    // search for and append if needed into the global params table
    struct qlist_head *ent;
    struct app_params *ap;
    qlist_for_each(ent, &app_params_list) {
        ap = qlist_entry(ent, struct app_params, ql);
        if (app_id == ap->app_id) {
            if (params_eq(&ap->params, p))
                break;
            else
                tw_error(TW_LOC, "app %d: rank %d passed in a different config",
                        app_id, rank);
        }
    }
    if (ent == &app_params_list) {
        ap = malloc(sizeof(*ap));
        assert(ap);
        ap->params = *p;
        ap->app_id = app_id;
        qlist_add_tail(&ap->ql, &app_params_list);
    }


    if (rank_tbl == NULL) {
        rank_tbl = qhash_init(rank_tbl_compare, quickhash_64bit_hash,
                p->rank_table_size ? p->rank_table_size : DEFAULT_HASH_TBL_SIZE);
        assert(rank_tbl);
    }

    struct rank_state *rs = malloc(sizeof(*rs));
    assert(rs);
    rs->rank = rank;
    rs->app_id = app_id;
    rs->has_opened = 0;
    rs->has_closed = 0;
    rs->reqs_processed = 0;
    rs->params = ap;

    qhash_add(rank_tbl, rs, &rs->hash_link);
    num_qhash_entries++;

    return 0;
}

static void iomock_workload_get_next(
        int app_id,
        int rank,
        struct codes_workload_op *op)
{
    struct rank_state cmp = { .rank = rank, .app_id = app_id };

    struct qhash_head *hash_link = qhash_search(rank_tbl, &cmp);
    if (hash_link == NULL)
        tw_error(TW_LOC, "mock workload: unable to find app-rank context for "
                "app %d, rank %d", app_id, rank);

    struct rank_state * rs =
        qhash_entry(hash_link, struct rank_state, hash_link);
    iomock_params const * p = &rs->params->params;

    if (!rs->has_opened) {
        op->op_type = CODES_WK_OPEN;
        op->u.open.create_flag = 1;
        op->u.open.file_id = get_file_id(p, rank);
        rs->has_opened = 1;
    }
    else if (rs->reqs_processed == p->num_requests) {
        if (!rs->has_closed) {
            op->op_type = CODES_WK_CLOSE;
            op->u.close.file_id = get_file_id(p, rank);
            rs->has_closed = 1;
        }
        else {
            op->op_type = CODES_WK_END;
            // cleanup
            qhash_del(hash_link);
            num_qhash_entries--;
            if (num_qhash_entries == 0) {
                qhash_finalize(rank_tbl);
                rank_tbl = NULL;
            }
        }
    }
    else {
        if (p->is_write) {
            op->op_type = CODES_WK_WRITE;
            op->u.write.file_id = get_file_id(p, rank);
            op->u.write.size = p->request_size;
            op->u.write.offset =
                (uint64_t)p->request_size * (uint64_t)rs->reqs_processed;
        }
        else {
            op->op_type = CODES_WK_READ;
            op->u.read.file_id = get_file_id(p, rank);
            op->u.read.size = p->request_size;
            op->u.read.offset =
                (uint64_t)p->request_size * (uint64_t)rs->reqs_processed;
        }
        rs->reqs_processed++;
    }
    return;
}

struct codes_workload_method iomock_workload_method =
{
    .method_name = "iomock_workload",
    .codes_workload_read_config = iomock_workload_read_config,
    .codes_workload_load = iomock_workload_load,
    .codes_workload_get_next = iomock_workload_get_next,
    .codes_workload_get_rank_cnt = NULL
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
