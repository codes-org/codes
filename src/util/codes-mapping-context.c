/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <codes/codes-mapping-context.h>
#include <codes/codes_mapping.h>

static struct codes_mctx const CODES_MCTX_DEFAULT_VAL = {
    .type = CODES_MCTX_GROUP_MODULO,
    .u = {
        .group_modulo = {
            .anno = {
                .annotation = NULL,
                .ignore_annotations = true
            }
        }
    }
};

struct codes_mctx const * const CODES_MCTX_DEFAULT = &CODES_MCTX_DEFAULT_VAL;

struct codes_mctx codes_mctx_set_global_direct(tw_lpid lpid)
{
    struct codes_mctx rtn;
    rtn.type = CODES_MCTX_GLOBAL_DIRECT;
    rtn.u.global_direct.lpid = lpid;
    return rtn;
}
struct codes_mctx codes_mctx_set_group_modulo(
        char const * annotation,
        bool ignore_annotations)
{
    struct codes_mctx rtn;
    rtn.type = CODES_MCTX_GROUP_MODULO;
    rtn.u.group_modulo.anno.annotation = annotation;
    rtn.u.group_modulo.anno.ignore_annotations = ignore_annotations;
    return rtn;
}
struct codes_mctx codes_mctx_set_group_direct(
        int offset,
        char const * annotation,
        bool ignore_annotations)
{
    struct codes_mctx rtn;
    rtn.type = CODES_MCTX_GROUP_DIRECT;
    rtn.u.group_direct.offset = offset;
    rtn.u.group_direct.anno.annotation = annotation;
    rtn.u.group_direct.anno.ignore_annotations = ignore_annotations;
    return rtn;
}

/* helper function to do a codes mapping - this function is subject to change
 * based on what types of ctx exist */
tw_lpid codes_mctx_to_lpid(
        struct codes_mctx const * ctx,
        char const * dest_lp_name,
        tw_lp const * sender)
{
    struct codes_mctx_annotation const *anno;
    // short circuit for direct mappings
    switch (ctx->type) {
        case CODES_MCTX_GLOBAL_DIRECT:
            return ctx->u.global_direct.lpid;
        case CODES_MCTX_GROUP_MODULO:
            anno = &ctx->u.group_modulo.anno;
            break;
        case CODES_MCTX_GROUP_DIRECT:
            anno = &ctx->u.group_direct.anno;
            break;
        default:
            assert(0);
    }

    char sender_group[MAX_NAME_LENGTH];
    int unused, rep_id, offset;

    // get sender info
    codes_mapping_get_lp_info(sender->gid, sender_group, &unused, NULL, &unused,
            NULL, &rep_id, &offset);

    int dest_offset;
    if (ctx->type == CODES_MCTX_GROUP_MODULO) {
        int num_dest_lps = codes_mapping_get_lp_count(sender_group, 1,
                dest_lp_name, anno->annotation, anno->ignore_annotations);
        if (num_dest_lps == 0)
            tw_error(TW_LOC,
                    "ERROR: Found no LPs of type %s in group %s "
                    "(source lpid %lu) with annotation: %s\n",
                    dest_lp_name, sender_group, sender->gid,
                    anno->ignore_annotations ? "ignored" :
                    (anno->annotation ? anno->annotation : "none"));

        dest_offset = offset % num_dest_lps;
    }
    else if (ctx->type == CODES_MCTX_GROUP_DIRECT) {
        dest_offset = ctx->u.group_direct.offset;
    }
    else
        assert(0);

    tw_lpid rtn;
    codes_mapping_get_lp_id(sender_group, dest_lp_name, anno->annotation,
            anno->ignore_annotations, rep_id, dest_offset, &rtn);
    return rtn;
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
