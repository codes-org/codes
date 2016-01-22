/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef __LS_MODEL__
#define __LS_MODEL__

#ifdef __cplusplus
extern "C" {
#endif

#include <ross.h>

#include "codes-callback.h"
#include "codes-mapping-context.h"

#define LSM_NAME "lsm"

/* HACK: problems arise when some LP sends multiple messages as part of an
 * event and expects FCFS ordering. One could simply set a higher delay in
 * delay, but that is hacky as well (and relies on knowing bounds on internal
 * codes_local_latency bounds. Hence, expose explicit start-sequence and
 * stop-sequence markers */
extern int lsm_in_sequence;
extern tw_stime lsm_msg_offset;
#define LSM_START_SEQ() do {\
    lsm_in_sequence = 1; \
    lsm_msg_offset = 0.0; \
} while (0)
#define LSM_END_SEQ() do {\
    lsm_in_sequence = 0;\
} while (0)

/*
 * lsm_event_t
 *   - events supported by the local storage model
 */
typedef enum lsm_event_e
{
    LSM_WRITE_REQUEST = 1,
    LSM_READ_REQUEST = 2,
    LSM_WRITE_COMPLETION = 3,
    LSM_READ_COMPLETION = 4
} lsm_event_t;

/*
 * return type for lsm events (in the codes-callback sense)
 */
typedef struct {
    int rc;
} lsm_return_t;

/*
 * Prototypes
 */

/* given LP sender, find the LSM device LP in the same group */ 
tw_lpid lsm_find_local_device(
        struct codes_mctx const * map_ctx,
        tw_lpid sender_gid);

/*
 * lsm_io_event
 *   - creates a new event that is targeted for the corresponding
 *     LSM LP.
 *   - this event will allow wrapping the callers completion event
 *   - lp_io_category: string name to identify the traffic category for use in
 *     lp-io
 *   - gid_offset: relative offset of the LSM LP to the originating LP
 *   - io_object: id of byte stream the caller will modify
 *   - io_offset: offset into byte stream
 *   - io_size_bytes: size in bytes of IO request
 *   - io_type: read or write request
 */

void lsm_io_event(
        const char * lp_io_category,
        uint64_t io_object,
        int64_t  io_offset,
        uint64_t io_size_bytes,
        int      io_type,
        tw_stime delay,
        tw_lp *sender,
        struct codes_mctx const * map_ctx,
        int return_tag,
        msg_header const * return_header,
        struct codes_cb_info const * cb);

void lsm_io_event_rc(tw_lp *sender);

/* get the priority count for the LSM scheduler.
 * returns 0 if priorities aren't being used, -1 if no LSMs were configured,
 * and >0 otherwise.
 * This should not be called before lsm_configure */
int lsm_get_num_priorities(
        struct codes_mctx const * map_ctx,
        tw_lpid sender_id);

/* set a request priority for the following lsm_event_*.
 * - tw_error will be called if the priority ends up being out-of-bounds
 *   (won't able to tell until the lsm_event call b/c annotations)
 * - not setting a priority (or setting a negative priority) is equivalent to
 *   setting the message to the lowest priority
 */
void lsm_set_event_priority(int prio);

/* registers the storage model LP with CODES/ROSS */
void lsm_register(void);

/* configures the LSM model(s) */
void lsm_configure(void);

#define LSM_DEBUG 0

#ifdef __cplusplus
}
#endif

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
