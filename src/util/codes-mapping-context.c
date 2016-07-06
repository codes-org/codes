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

static struct codes_mctx set_group_modulo_common(
        enum codes_mctx_type type,
        char const * annotation,
        bool ignore_annotations)
{
    struct codes_mctx rtn;
    rtn.type = type;
    if (ignore_annotations)
        rtn.u.group_modulo.anno.cid = -1;
    else
        rtn.u.group_modulo.anno.cid =
            codes_mapping_get_anno_cid_by_name(annotation);
    return rtn;
}

struct codes_mctx codes_mctx_set_group_modulo(
        char const * annotation,
        bool ignore_annotations)
{
    return set_group_modulo_common(CODES_MCTX_GROUP_MODULO, annotation,
            ignore_annotations);
}

struct codes_mctx codes_mctx_set_group_modulo_reverse(
        char const * annotation,
        bool ignore_annotations)
{
    return set_group_modulo_common(CODES_MCTX_GROUP_MODULO_REVERSE, annotation,
            ignore_annotations);
}

static struct codes_mctx set_group_ratio_common(
        enum codes_mctx_type type,
        char const * annotation,
        bool ignore_annotations)
{
    struct codes_mctx rtn;
    rtn.type = type;
    if (ignore_annotations)
        rtn.u.group_ratio.anno.cid = -1;
    else
        rtn.u.group_ratio.anno.cid =
            codes_mapping_get_anno_cid_by_name(annotation);
    return rtn;
}

struct codes_mctx codes_mctx_set_group_ratio(
        char const * annotation,
        bool ignore_annotations)
{
    return set_group_ratio_common(CODES_MCTX_GROUP_RATIO, annotation,
            ignore_annotations);
}

struct codes_mctx codes_mctx_set_group_ratio_reverse(
        char const * annotation,
        bool ignore_annotations)
{
    return set_group_ratio_common(CODES_MCTX_GROUP_RATIO_REVERSE, annotation,
            ignore_annotations);
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
        case CODES_MCTX_GROUP_RATIO:
        case CODES_MCTX_GROUP_RATIO_REVERSE:
            anno = &ctx->u.group_ratio.anno;
            break;
        case CODES_MCTX_GROUP_MODULO:
        case CODES_MCTX_GROUP_MODULO_REVERSE:
            anno = &ctx->u.group_modulo.anno;
            break;
        case CODES_MCTX_GROUP_DIRECT:
            anno = &ctx->u.group_direct.anno;
            break;
        default:
            assert(0);
    }

    char const *sender_group;
    char const *sender_lpname;
    int rep_id, offset;

    // get sender info
    codes_mapping_get_lp_info2(sender_gid, &sender_group, &sender_lpname, NULL,
            &rep_id, &offset);

    char const * anno_str;
    if (anno->cid < 0)
        anno_str = NULL;
    else
        anno_str = codes_mapping_get_anno_name_by_cid(anno->cid);

    int dest_offset;
    int is_group_modulo = (ctx->type == CODES_MCTX_GROUP_MODULO ||
            ctx->type == CODES_MCTX_GROUP_MODULO_REVERSE);
    int is_group_ratio = (ctx->type == CODES_MCTX_GROUP_RATIO ||
            ctx->type == CODES_MCTX_GROUP_RATIO_REVERSE);
    int is_group_reverse = (ctx->type == CODES_MCTX_GROUP_MODULO_REVERSE ||
            ctx->type == CODES_MCTX_GROUP_RATIO_REVERSE);

    if (is_group_modulo || is_group_ratio) {
        int num_dest_lps = codes_mapping_get_lp_count(sender_group, 1,
                dest_lp_name, anno_str, anno->cid == -1);
        if (num_dest_lps == 0)
            tw_error(TW_LOC,
                    "ERROR: Found no LPs of type %s in group %s "
                    "(source lpid %lu) with annotation: %s\n",
                    dest_lp_name, sender_group, sender_gid,
                    anno->cid == -1 ? "ignored" :
                    codes_mapping_get_anno_name_by_cid(anno->cid));

        if (is_group_modulo)
            dest_offset = offset % num_dest_lps;
        else {
            int num_src_lps = codes_mapping_get_lp_count(sender_group, 1,
                    sender_lpname, NULL, 1);
            if (num_src_lps <= num_dest_lps)
                dest_offset = offset;
            else {
                dest_offset = offset * num_dest_lps / num_src_lps;
                if (dest_offset >= num_dest_lps)
                    dest_offset = num_dest_lps-1;
            }
        }
        if (is_group_reverse)
            dest_offset = num_dest_lps - 1 - dest_offset;
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
        case CODES_MCTX_GROUP_RATIO:
        case CODES_MCTX_GROUP_RATIO_REVERSE:
            // if not ignoring the annotation, just return what's in the
            // context
            if (ctx->u.group_modulo.anno.cid >= 0)
                return codes_mapping_get_anno_name_by_cid(
                        ctx->u.group_modulo.anno.cid);
            break;
        case CODES_MCTX_GROUP_MODULO:
        case CODES_MCTX_GROUP_MODULO_REVERSE:
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
