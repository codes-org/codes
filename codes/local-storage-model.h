/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef __LS_MODEL__
#define __LS_MODEL__

#include <ross.h>

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
 * Prototypes
 */

/* given LP sender, find the LSM device LP in the same group */ 
tw_lpid lsm_find_local_device(
        const char * annotation,
        int ignore_annotations,
        tw_lpid sender_gid);

/*
 * lsm_event_new
 *   - creates a new event that is targeted for the corresponding
 *     LSM LP.
 *   - this event will allow wrapping the callers completion event
 *   - category: string name to identify the traffic category
 *   - dest_gid: the gid to send the callers event to
 *   - gid_offset: relative offset of the LSM LP to the originating LP
 *   - io_object: id of byte stream the caller will modify
 *   - io_offset: offset into byte stream
 *   - io_size_bytes: size in bytes of IO request
 *   - io_type: read or write request
 *   - message_bytes: size of the event message the caller will have
 *   - sender: id of the sender
 */
tw_event* lsm_event_new(const char* category,
                        tw_lpid  dest_gid,
                        uint64_t io_object,
                        int64_t  io_offset,
                        uint64_t io_size_bytes,
                        int      io_type,
                        size_t   message_bytes,
                        tw_lp   *sender,
                        tw_stime delay);
/* equivalent to lsm_event_new, except it allows to specify an annotation to
 * filter by. If ignore_annotations is nonzero, A null annotation parameter
 * indicates that the lsm LP to issue to has no annotation */
tw_event* lsm_event_new_annotated(
        const char* category,
        tw_lpid  dest_gid,
        uint64_t io_object,
        int64_t  io_offset,
        uint64_t io_size_bytes,
        int      io_type,
        size_t   message_bytes,
        tw_lp   *sender,
        tw_stime delay,
        const char * annotation,
        int ignore_annotations);

void lsm_event_new_reverse(tw_lp *sender);

/*
 * lsm_event_data
 *   - returns the pointer to the message data for the callers data
 *   - event: a lsm_event_t event
 */
void* lsm_event_data(tw_event *event);

/* registers the storage model LP with CODES/ROSS */
void lsm_register(void);

/* configures the LSM model(s) */
void lsm_configure(void);

/*
 * Macros
 */
#define lsm_write_event_new(cat,gid,obj,off,sz,mb,s) \
  lsm_event_new((cat),(gid),(obj),(off),(sz),LSM_WRITE_REQUEST,(mb),(s),0.0)

#define lsm_read_event_new(cat,gid,obj,off,sz,mb,s) \
  lsm_event_new((cat),(gid),(obj),(off),(sz),LSM_READ_REQUEST,(mb),(s),0.0)

#define LSM_DEBUG 0

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
