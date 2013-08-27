/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/*
 * timeline.h
 *
 *  Created on: Jun 24, 2013
 *      Author: wozniak
 */

#ifndef TIMELINE_H
#define TIMELINE_H

#define TIMELINE_ENABLED 0

#if TIMELINE_ENABLED == 1

#include <ross.h>

/**
   Open filename for writing for timeline data
   @return 1 on success, 0 on error
 */
int timeline_init(const char *filename);

/**
   Write a timeline record
   @return 1 on success, 0 on error
 */
int timeline_printf(const char *format, ...);

/**
   Write a timeline record
   @return 1 on success, 0 on error
 */
int timeline_printf_va(const char *format, va_list ap);

/**
   Write a timeline record with typical ROSS metadata
   @return 1 on success, 0 on error
 */
#define timeline_event(lp, format, args...) \
    timeline_event_impl(lp, __func__, format, ##args)

int timeline_event_impl(const tw_lp *lp, const char *func,
                        const char *format, ...);

/**
   Finalize the timeline module
 */
void timeline_finalize(void);

#else

// Set all functions to noops (return success value 1)
#define timeline_init(x)            1
#define timeline_printf(f, a...)    1
#define timeline_printf_va(f, a)    1
#define timeline_event(lp, f, a...) 1
#define timeline_finalize()

#endif

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
