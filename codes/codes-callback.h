/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

#ifndef CODES_CALLBACK_H
#define CODES_CALLBACK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include "codes/lp-msg.h"

/* This header defines the following conventions for callback-based event
 * processing (i.e., RPC-like event flow where the callee sends a "return
 * event" to the caller)
 *
 * callers:
 * - give the caller event size (for sanity checking)
 * - use the msg_header convention (lp-msg.h)
 * - specify an integer tag to identify which call the callee return event
 *   corresponds to, in the case of multiple pending callbacks
 * - specify an offset to the return structure, which is defined by the callees
 *
 * callees:
 * - use the resulting set of offsets to write and issue the callback
 *
 * The recommended calling convention for these types of event flows is:
 * void event_func(<params...>,
 *         msg_header const * h, int tag, struct codes_cb_info const * cb);
 */

struct codes_cb_info {
    int event_size;
    int header_offset;
    int tag_offset;
    int cb_ret_offset;
};

/* helper struct for callees - wrap up the typical inputs */
struct codes_cb_params {
    struct codes_cb_info info;
    msg_header h;
    int tag;
};

/* function-like sugar for initializing a codes_cb_info */
#define INIT_CODES_CB_INFO(_cb_info_ptr, _event_type, _header_field, _tag_field, _cb_ret_field) \
    do { \
        (_cb_info_ptr)->event_size    = sizeof(_event_type); \
        (_cb_info_ptr)->header_offset = offsetof(_event_type, _header_field); \
        (_cb_info_ptr)->tag_offset    = offsetof(_event_type, _tag_field); \
        (_cb_info_ptr)->cb_ret_offset = offsetof(_event_type, _cb_ret_field); \
    } while (0)

#define CB_HO(_cb_params_ptr) ((_cb_params_ptr)->info.header_offset)
#define CB_TO(_cb_params_ptr) ((_cb_params_ptr)->info.tag_offset)
#define CB_RO(_cb_params_ptr) ((_cb_params_ptr)->info.cb_ret_offset)

/* Declare return variables at the right byte offsets from a codes_cb_params.
 * Additionally, set the header and tag vars */
#define GET_INIT_CB_PTRS(_cb_params_ptr, _data_ptr, _sender_gid_val, _header_nm, _tag_nm, _rtn_name, _rtn_type) \
    msg_header * _header_nm = \
            (msg_header*)((char*)(_data_ptr) + CB_HO(_cb_params_ptr)); \
    int * _tag_nm = \
            (int*)((char*)(_data_ptr) + CB_TO(_cb_params_ptr));\
    _rtn_type * _rtn_name = \
            (_rtn_type*)((char*)(_data_ptr) + CB_RO(_cb_params_ptr)); \
    msg_set_header((_cb_params_ptr)->h.magic, (_cb_params_ptr)->h.event_type, \
            _sender_gid_val, _header_nm); \
    *_tag_nm = (_cb_params_ptr)->tag

#define SANITY_CHECK_CB(_cb_info_ptr, _ret_type) \
    do { \
        assert((_cb_info_ptr)->event_size > 0 && \
                (_cb_info_ptr)->header_offset >= 0 && \
                (_cb_info_ptr)->tag_offset >= 0 && \
                (_cb_info_ptr)->cb_ret_offset >= 0); \
        size_t _total_size = sizeof(_ret_type) + sizeof(int) + sizeof(msg_header);\
        size_t _esize = (_cb_info_ptr)->event_size; \
        assert(_esize >= _total_size); \
        assert(_esize >= (_cb_info_ptr)->header_offset + sizeof(msg_header)); \
        assert(_esize >= (_cb_info_ptr)->tag_offset + sizeof(int)); \
        assert(_esize >= (_cb_info_ptr)->cb_ret_offset + sizeof(_ret_type)); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* end of include guard: CODES_CALLBACK_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
