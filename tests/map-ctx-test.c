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

    struct codes_mctx direct, group_modulo, group_rmodulo, group_direct,
                      group_modulo_anno, group_rmodulo_anno, group_direct_anno;

    direct              = codes_mctx_set_global_direct(12ul);
    group_modulo        = codes_mctx_set_group_modulo(NULL, true);
    group_rmodulo       = codes_mctx_set_group_modulo_reverse(NULL, true);
    group_direct        = codes_mctx_set_group_direct(1, NULL, true);
    group_modulo_anno   = codes_mctx_set_group_modulo("baz", false);
    group_rmodulo_anno  = codes_mctx_set_group_modulo_reverse("baz", false);
    group_direct_anno   = codes_mctx_set_group_direct(1, "baz", false);

    tw_lpid in;
    tw_lpid rtn_id;
    char const * rtn_anno;

    rtn_id = codes_mctx_to_lpid(&direct, NULL, 0);
    if (12ul != rtn_id)
        ERR("global_direct mapping: expected %lu, got %lu",
                12ul, rtn_id);
    rtn_anno = codes_mctx_get_annotation(&direct, NULL, 0);
    if (rtn_anno)
        ERR("global_direct mapping: expected NULL anno, got %s", rtn_anno);

    in = 8ul;
    rtn_id = codes_mctx_to_lpid(&group_modulo, "bar", in);
    if (rtn_id != 9ul)
        ERR("group_modulo mapping: expected %lu, got %lu",
                9ul, rtn_id);
    rtn_anno = codes_mctx_get_annotation(&group_modulo, "bar", in);
    if (rtn_anno)
        ERR("group_modulo mapping: expected NULL anno, got %s", rtn_anno);

    rtn_id = codes_mctx_to_lpid(&group_rmodulo, "bar", in);
    if (rtn_id != 9ul)
        ERR("group_rmodulo mapping: expected %lu, got %lu",
                9ul, rtn_id);
    rtn_anno = codes_mctx_get_annotation(&group_rmodulo, "bar", in);
    if (rtn_anno)
        ERR("group_rmodulo mapping: expected NULL anno, got %s", rtn_anno);

    in = 12ul;
    rtn_id = codes_mctx_to_lpid(&group_modulo, "bar", in);
    if (rtn_id != 13ul)
        ERR("group_modulo mapping: expected %lu, got %lu",
                13ul, rtn_id);
    rtn_anno = codes_mctx_get_annotation(&group_modulo, "bar", in);
    if (rtn_anno)
        ERR("group_modulo mapping: expected NULL anno, got %s", rtn_anno);

    rtn_id = codes_mctx_to_lpid(&group_rmodulo, "bar", in);
    if (rtn_id != 14ul)
        ERR("group_rmodulo mapping: expected %lu, got %lu",
                14ul, rtn_id);
    rtn_anno = codes_mctx_get_annotation(&group_rmodulo, "bar", in);
    if (rtn_anno)
        ERR("group_rmodulo mapping: expected NULL anno, got %s", rtn_anno);

    rtn_id = codes_mctx_to_lpid(CODES_MCTX_DEFAULT, "bar", in);
    if (rtn_id != 13ul)
        ERR("group_modulo mapping (default): expected %lu, got %lu",
                13ul, rtn_id);
    rtn_anno = codes_mctx_get_annotation(CODES_MCTX_DEFAULT, "bar", in);
    if (rtn_anno)
        ERR("group_modulo mapping: expected NULL anno, got %s", rtn_anno);

    rtn_id = codes_mctx_to_lpid(&group_modulo_anno, "bar", in);
    if (rtn_id != 15ul)
        ERR("group_modulo annotated mapping: expected %lu, got %lu",
                15ul, rtn_id);
    rtn_anno = codes_mctx_get_annotation(&group_modulo_anno, "bar", in);
    if (strcmp(rtn_anno,"baz") != 0)
        ERR("group_modulo mapping: expected anno \"baz\", got %s", rtn_anno);

    rtn_id = codes_mctx_to_lpid(&group_rmodulo_anno, "bar", in);
    if (rtn_id != 16ul)
        ERR("group_rmodulo annotated mapping: expected %lu, got %lu",
                16ul, rtn_id);
    rtn_anno = codes_mctx_get_annotation(&group_rmodulo_anno, "bar", in);
    if (strcmp(rtn_anno,"baz") != 0)
        ERR("group_rmodulo mapping: expected anno \"baz\", got %s", rtn_anno);

    in = 10ul;
    rtn_id = codes_mctx_to_lpid(&group_direct, "bar", in);
    if (rtn_id != 14ul)
        ERR("group_direct mapping (default): expected %lu, got %lu",
                14ul, rtn_id);
    rtn_anno = codes_mctx_get_annotation(&group_direct, "bar", in);
    if (rtn_anno)
        ERR("group_modulo mapping: expected NULL anno, got %s", rtn_anno);

    rtn_id = codes_mctx_to_lpid(&group_direct_anno, "bar", in);
    if (rtn_id != 16ul)
        ERR("group_direct mapping (default): expected %lu, got %lu",
                16ul, rtn_id);
    rtn_anno = codes_mctx_get_annotation(&group_direct_anno, "bar", in);
    if (strcmp(rtn_anno,"baz") != 0)
        ERR("group_modulo mapping: expected anno \"baz\", got %s", rtn_anno);

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
