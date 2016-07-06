/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY - data structure and utilities to direct LP mappings. As opposed to
 * the codes-mapping API, this defines metadata necessary to allow implicit
 * mappings based on LP type and configuration specification
 * (see modelnet, LSM, etc)
 * mctx stands for mapping context */

#ifndef CODES_MAPPING_CONTEXT_H
#define CODES_MAPPING_CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <ross.h>

/* for convenience - an annotation-ignoring "group_modulo" context,
 * matching previous mapping behavior in most interfaces (modelnet and such) */
extern struct codes_mctx const * const CODES_MCTX_DEFAULT;

/* types of map contexts */
enum codes_mctx_type {
    // instructs those using the context to map directly to an LP
    CODES_MCTX_GLOBAL_DIRECT,
    // instructs those using the context to map into the same group/repetition
    // and compute the callee offset taking into account the ratio of source LP
    // type count and destination LP type count. Currently doesn't respect
    // annotations from the source LP.
    CODES_MCTX_GROUP_RATIO,
    // similar to GROUP_RATIO, but maps to offsets in reverse order
    CODES_MCTX_GROUP_RATIO_REVERSE,
    // instructs those using the context to map into the same group/repetition
    // and compute the callee offset as the modulus of the caller offset and
    // the number of callees in the group, to provide simple wraparound
    // behaviour
    CODES_MCTX_GROUP_MODULO,
    // similar to GROUP_MODULO, but maps to offsets in reverse order
    CODES_MCTX_GROUP_MODULO_REVERSE,
    // instructs those using the context to map into the same group/repetition
    // and directly to a callee offset
    CODES_MCTX_GROUP_DIRECT,
    // unknown/uninitialized context
    CODES_MCTX_UNKNOWN
};

/* defines whether to specialize by destination annotation, and if so, which
 * one */
struct codes_mctx_annotation {
    // see canonical name mapping api in codes_mapping.h. -1 is used for
    // ignoring annotations
    int cid;
};

/* parameters for each mapping context type */
struct codes_mctx_global_direct {
    tw_lpid lpid;
};

struct codes_mctx_group_ratio {
    struct codes_mctx_annotation anno;
};

struct codes_mctx_group_modulo {
    struct codes_mctx_annotation anno;
};
// NOTE: group_modulo_reverse shares the group_modulo representation

struct codes_mctx_group_direct {
    struct codes_mctx_annotation anno;
    int offset;
};

struct codes_mctx {
    enum codes_mctx_type type;
    union {
        struct codes_mctx_global_direct global_direct;
        struct codes_mctx_group_ratio    group_ratio;
        struct codes_mctx_group_modulo  group_modulo;
        struct codes_mctx_group_direct  group_direct;
    } u;
};

/* simple setter functions */
struct codes_mctx codes_mctx_set_global_direct(tw_lpid lpid);

struct codes_mctx codes_mctx_set_group_ratio(
        char const * annotation,
        bool ignore_annotations);

struct codes_mctx codes_mctx_set_group_ratio_reverse(
        char const * annotation,
        bool ignore_annotations);

struct codes_mctx codes_mctx_set_group_modulo(
        char const * annotation,
        bool ignore_annotations);

struct codes_mctx codes_mctx_set_group_modulo_reverse(
        char const * annotation,
        bool ignore_annotations);

struct codes_mctx codes_mctx_set_group_direct(
        int offset,
        char const * annotation,
        bool ignore_annotations);

/* helper function to do a codes mapping - this function is subject to change
 * based on what types of ctx exist
 * NOTE: in GLOBAL_DIRECT mode, dest_lp_name and sender_gid are ignored */
tw_lpid codes_mctx_to_lpid(
        struct codes_mctx const * ctx,
        char const * dest_lp_name,
        tw_lpid sender_gid);

/* helper function to extract which annotation a various map context maps to.
 * annotation is allocated or NULL if unused */
char const * codes_mctx_get_annotation(
        struct codes_mctx const *ctx,
        char const * dest_lp_name,
        tw_lpid sender_id);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard: CODES_MAPPING_CONTEXT_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
