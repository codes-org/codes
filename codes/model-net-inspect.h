/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* This header provides ways for modelnet users to look at the state of the
 * underlying implementations (e.g., torus) */

#ifndef MODEL_NET_INSPECT_H
#define MODEL_NET_INSPECT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ALL FUNCTIONS
 * anno is the annotation specified in the configuration (NULL -> no
 * annotation), while ignore_annotations is a flag controlling whether
 * annotations are checked for or not - if not, then the first matching LP type
 * found is used irrespective of annotation. */

/** TORUS FUNCTIONS **/

/* get the dimensions of a torus configuration torus network. n and dims are
 * return paramters */
void model_net_torus_get_dims(
        char const        * anno,
        int                 ignore_annotations,
        int               * n,
        int const * const * dims);

/* mapping utilities to and from linearized torus node ids */
void model_net_torus_get_dim_id(
        int         flat_id,
        int         ndims,
        const int * dim_lens,
        int       * out_dim_ids);

int model_net_torus_get_flat_id(
        int         ndims,
        const int * dim_lens,
        const int * dim_ids);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard: MODEL_NET_INSPECT_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
