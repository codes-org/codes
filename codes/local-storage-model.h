/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef __LS_MODEL__
#define __LS_MODEL__

#include <ross.h>

#define LSM_NAME "lsm"

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
void lsm_event_new_reverse(tw_lp *sender);

tw_lpid lsm_find_local_device(tw_lp *sender);

tw_event* lsm_event_new(const char* category,
                        tw_lpid  dest_gid,
                        uint64_t io_object,
                        int64_t  io_offset,
                        uint64_t io_size_bytes,
                        int      io_type,
                        size_t   message_bytes,
                        tw_lp   *sender,
                        tw_stime delay);

void* lsm_event_data(tw_event *event);

void lsm_register(void);
void lsm_configure(void);

void lsm_send_init (tw_lpid gid, tw_lp *lp, char *name);

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
