/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/*
 * timeline.c
 *
 *  Created on: Jun 24, 2013
 *      Author: wozniak
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codes/timeline.h"

// If timeline is not enabled, all functions are defined to noops
#if TIMELINE_ENABLED == 1

static char* name = NULL;
static FILE* fp = NULL;

int timeline_init(const char *filename)
{
    name = strdup(filename);
    fp = fopen(filename, "w");
    if (fp == NULL)
    {
        perror(__func__);
        return 0;
    }
    return 1;
}

int timeline_printf(const char *format, ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);
    rc = timeline_printf_va(format, ap);
    va_end(ap);

    return rc;
}

int timeline_printf_va(const char *format, va_list ap)
{
    int rc;
    if (fp == NULL)
    {
        printf("timeline module is not initialized!\n");
        exit(1);
    }
    rc = vfprintf(fp, format, ap);
    if (rc < 0) return 0;
    return 1;
}

int timeline_event_impl(const tw_lp *lp, const char *func,
                        const char *format, ...)
{
    int rc;
    va_list ap;
    rc = timeline_printf("%4i %016.6f %-24s",
                         lp->gid, lp->kp->last_time, func);
    if (rc == 0) return 0;
    va_start(ap, format);
    rc = timeline_printf_va(format, ap);
    if (rc == 0) return 0;
    va_end(ap);
    return 1;
}

void timeline_finalize()
{
    assert(fp != NULL);
    fclose(fp);
    printf("wrote timeline to: %s\n", name);
    free(name);
}

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
