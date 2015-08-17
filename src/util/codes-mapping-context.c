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
                .cid = -1,
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
    if (ignore_annotations)
        rtn.u.group_modulo.anno.cid = -1;
    else
        rtn.u.group_modulo.anno.cid =
            codes_mapping_get_anno_cid_by_name(annotation);
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
    if (ignore_annotations)
        rtn.u.group_direct.anno.cid = -1;
    else
        rtn.u.group_direct.anno.cid =
            codes_mapping_get_anno_cid_by_name(annotation);
    return rtn;
}

/* helper function to do a codes mapping - this function is subject to change
 * based on what types of ctx exist */
tw_lpid codes_mctx_to_lpid(
        struct codes_mctx const * ctx,
        char const * dest_lp_name,
        tw_lpid sender_gid)
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
    codes_mapping_get_lp_info(sender_gid, sender_group, &unused, NULL, &unused,
            NULL, &rep_id, &offset);

    char const * anno_str;
    if (anno->cid < 0)
        anno_str = NULL;
    else
        anno_str = codes_mapping_get_anno_name_by_cid(anno->cid);

    int dest_offset;
    if (ctx->type == CODES_MCTX_GROUP_MODULO) {
        int num_dest_lps = codes_mapping_get_lp_count(sender_group, 1,
                dest_lp_name, anno_str, anno->cid == -1);
        if (num_dest_lps == 0)
            tw_error(TW_LOC,
                    "ERROR: Found no LPs of type %s in group %s "
                    "(source lpid %lu) with annotation: %s\n",
                    dest_lp_name, sender_group, sender_gid,
                    anno->cid == -1 ? "ignored" :
                    codes_mapping_get_anno_name_by_cid(anno->cid));

        dest_offset = offset % num_dest_lps;
    }
    else if (ctx->type == CODES_MCTX_GROUP_DIRECT) {
        dest_offset = ctx->u.group_direct.offset;
    }
    else
        assert(0);

    tw_lpid rtn;
    codes_mapping_get_lp_id(sender_group, dest_lp_name, anno_str,
            anno->cid == -1, rep_id, dest_offset, &rtn);
    return rtn;
}

char const * codes_mctx_get_annotation(
        struct codes_mctx const *ctx,
        char const * dest_lp_name,
        tw_lpid sender_id)
{
    switch(ctx->type) {
        case CODES_MCTX_GLOBAL_DIRECT:
            return codes_mapping_get_annotation_by_lpid(sender_id);
        case CODES_MCTX_GROUP_MODULO:
            // if not ignoring the annotation, just return what's in the
            // context
            if (ctx->u.group_modulo.anno.cid >= 0)
                return codes_mapping_get_anno_name_by_cid(
                        ctx->u.group_modulo.anno.cid);
            break;
        case CODES_MCTX_GROUP_DIRECT:
            if (ctx->u.group_direct.anno.cid >= 0)
                return codes_mapping_get_anno_name_by_cid(
                        ctx->u.group_direct.anno.cid);
            break;
        default:
            tw_error(TW_LOC, "unrecognized or uninitialized context type: %d",
                    ctx->type);
            return NULL;
    }
    // at this point, we must be a group-wise mapping ignoring annotations

    char group[MAX_NAME_LENGTH];
    int dummy;
    // only need the group name
    codes_mapping_get_lp_info(sender_id, group, &dummy, NULL, &dummy, NULL,
            &dummy, &dummy);

    return codes_mapping_get_annotation_by_name(group, dest_lp_name);
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
