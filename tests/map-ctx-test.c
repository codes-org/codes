/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdbool.h>
#include <mpi.h>
#include <codes/codes-mapping-context.h>
#include <codes/configuration.h>
#include <codes/codes.h>

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

    struct codes_mctx direct, group_ratio, group_rratio, group_modulo,
                      group_rmodulo, group_direct, group_modulo_anno,
                      group_rmodulo_anno, group_direct_anno;

    direct              = codes_mctx_set_global_direct(12ul);
    group_ratio         = codes_mctx_set_group_ratio(NULL, true);
    group_rratio        = codes_mctx_set_group_ratio_reverse(NULL, true);
    group_modulo        = codes_mctx_set_group_modulo(NULL, true);
    group_rmodulo       = codes_mctx_set_group_modulo_reverse(NULL, true);
    group_direct        = codes_mctx_set_group_direct(1, NULL, true);
    group_modulo_anno   = codes_mctx_set_group_modulo("baz", false);
    group_rmodulo_anno  = codes_mctx_set_group_modulo_reverse("baz", false);
    group_direct_anno   = codes_mctx_set_group_direct(1, "baz", false);

    tw_lpid in, out;
    tw_lpid rtn_id;
    char const * out_anno;
    char const * rtn_anno;

#define CHECK(_type_str) \
    do { \
        if (rtn_id != out) { \
            ERR("%s mapping failed: in:%llu, expected:%llu, out:%llu", \
                    _type_str, LLU(in), LLU(rtn_id), LLU(out)); \
        } \
    } while(0)

#define CHECK_ANNO(_type_str) \
    do { \
        if (!((out_anno && rtn_anno && strcmp(out_anno, rtn_anno) == 0) || \
                (!out_anno && !rtn_anno))) { \
            ERR("%s anno mapping failed: in:%llu, expected:%s, out:%s", \
                    _type_str, LLU(in), rtn_anno, out_anno); \
        } \
    } while (0)


    in  = 0ul;
    out = 12ul;
    out_anno = NULL;
    rtn_id = codes_mctx_to_lpid(&direct, NULL, 0);
    rtn_anno = codes_mctx_get_annotation(&direct, NULL, 0);
    CHECK("global_direct");
    CHECK_ANNO("global_direct");

    /* test BAZ group (evenly divide foo, bar) */
    in = 47ul;
    out = 50ul;
    out_anno = NULL;
    rtn_id = codes_mctx_to_lpid(&group_ratio, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_ratio, "bar", in);
    CHECK("global_ratio");
    CHECK_ANNO("global_ratio");

    out = 49ul;
    rtn_id = codes_mctx_to_lpid(&group_rratio, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_rratio, "bar", in);
    CHECK("global_rratio");
    CHECK_ANNO("global_rratio");

    /* test BAT group (non-even foo/bar division) */
    in = 79ul;
    out = 82ul;
    out_anno = NULL;
    rtn_id = codes_mctx_to_lpid(&group_ratio, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_ratio, "bar", in);
    CHECK("global_ratio");
    CHECK_ANNO("global_ratio");

    out = 80ul;
    rtn_id = codes_mctx_to_lpid(&group_rratio, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_rratio, "bar", in);
    CHECK("global_ratio");
    CHECK_ANNO("global_ratio");

    in = 8ul;
    out = 9ul;
    out_anno = NULL;
    rtn_id = codes_mctx_to_lpid(&group_modulo, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_modulo, "bar", in);
    CHECK("group_modulo");
    CHECK_ANNO("group_modulo");

    rtn_id = codes_mctx_to_lpid(&group_rmodulo, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_rmodulo, "bar", in);
    CHECK("group_rmodulo");
    CHECK_ANNO("group_rmodulo");

    in = 12ul;
    out = 13ul;
    out_anno = NULL;
    rtn_id = codes_mctx_to_lpid(&group_modulo, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_modulo, "bar", in);
    CHECK("group_modulo");
    CHECK_ANNO("group_modulo");

    out = 14ul;
    rtn_id = codes_mctx_to_lpid(&group_rmodulo, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_rmodulo, "bar", in);
    CHECK("group_rmodulo");
    CHECK_ANNO("group_rmodulo");

    out = 13ul;
    rtn_id = codes_mctx_to_lpid(CODES_MCTX_DEFAULT, "bar", in);
    rtn_anno = codes_mctx_get_annotation(CODES_MCTX_DEFAULT, "bar", in);
    CHECK("CODES_MCTX_DEFAULT");
    CHECK_ANNO("CODES_MCTX_DEFAULT");

    out = 15ul;
    out_anno = "baz";
    rtn_id = codes_mctx_to_lpid(&group_modulo_anno, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_modulo_anno, "bar", in);
    CHECK("group_modulo_anno");
    CHECK_ANNO("group_modulo_anno");

    out = 16ul;
    rtn_id = codes_mctx_to_lpid(&group_rmodulo_anno, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_rmodulo_anno, "bar", in);
    CHECK("group_rmodulo_anno");
    CHECK_ANNO("group_rmodulo_anno");

    in = 10ul;
    out = 14ul;
    out_anno = NULL;
    rtn_id = codes_mctx_to_lpid(&group_direct, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_direct, "bar", in);
    CHECK("group_direct");
    CHECK_ANNO("group_direct");

    out = 16ul;
    out_anno = "baz";
    rtn_id = codes_mctx_to_lpid(&group_direct_anno, "bar", in);
    rtn_anno = codes_mctx_get_annotation(&group_direct_anno, "bar", in);
    CHECK("group_direct_anno");
    CHECK("group_direct_anno");

    MPI_Finalize();

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
