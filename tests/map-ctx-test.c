/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdbool.h>
#include <mpi.h>
#include <codes/codes-mapping-context.h>
#include <codes/configuration.h>

#define ERR(_fmt, ...) \
    do { \
        fprintf(stderr, "Error at %s:%d: " _fmt "\n", __FILE__, __LINE__, \
                ##__VA_ARGS__); \
        return 1; \
    } while (0)

/* NOTE: hard-coded against configuration file */
int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rc = configuration_load(argv[1], MPI_COMM_WORLD, &config);
    if (rc != 0)
        ERR("unable to load configuration file %s", argv[1]);

    struct codes_mctx direct, group_modulo, group_direct,
                      group_modulo_anno, group_direct_anno;

    direct            = codes_mctx_set_global_direct(12ul);
    group_modulo      = codes_mctx_set_group_modulo(NULL, true);
    group_direct      = codes_mctx_set_group_direct(1, NULL, true);
    group_modulo_anno = codes_mctx_set_group_modulo("baz", false);
    group_direct_anno = codes_mctx_set_group_direct(1, "baz", false);

    tw_lp mock_lp;
    tw_lpid rtn;

    rtn = codes_mctx_to_lpid(&direct, NULL, NULL);
    if (12ul != rtn)
        ERR("global_direct mapping: expected %lu, got %lu", 12ul, rtn);

    mock_lp.gid = 8ul;
    rtn = codes_mctx_to_lpid(&group_modulo, "bar", &mock_lp);
    if (rtn != 9ul)
        ERR("group_modulo mapping: expected %lu, got %lu", 9ul, rtn);

    mock_lp.gid = 12ul;
    rtn = codes_mctx_to_lpid(&group_modulo, "bar", &mock_lp);
    if (rtn != 13ul)
        ERR("group_modulo mapping: expected %lu, got %lu", 13ul, rtn);

    rtn = codes_mctx_to_lpid(CODES_MCTX_DEFAULT, "bar", &mock_lp);
    if (rtn != 13ul)
        ERR("group_modulo mapping (default): expected %lu, got %lu", 13ul, rtn);

    rtn = codes_mctx_to_lpid(&group_modulo_anno, "bar", &mock_lp);
    if (rtn != 15ul)
        ERR("group_modulo annotated mapping: expected %lu, got %lu", 15ul, rtn);

    mock_lp.gid = 10ul;
    rtn = codes_mctx_to_lpid(&group_direct, "bar", &mock_lp);
    if (rtn != 14ul)
        ERR("group_direct mapping (default): expected %lu, got %lu", 14ul, rtn);

    rtn = codes_mctx_to_lpid(&group_direct_anno, "bar", &mock_lp);
    if (rtn != 16ul)
        ERR("group_direct mapping (default): expected %lu, got %lu", 16ul, rtn);

    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
