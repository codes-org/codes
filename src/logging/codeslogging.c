/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/** \file
 *  \ingroup codeslogging
 *
 *  Implementation of codeslogging interface.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#include "codes/codeslogging.h"
#include <pthread.h>

/** controls whether debugging is on or off */
int codeslogging_debug_on = 0;

/** controls the mask level for debugging messages */
uint64_t codeslogging_debug_mask = 0;

enum
{
    CODESLOGGING_STDERR = 1,
    CODESLOGGING_FILE = 2,
};

/** determines which logging facility to use.  Default to stderr to begin
 *  with.
 */
int codeslogging_facility = CODESLOGGING_STDERR;

/* file handle used for file logging */
static FILE *internal_log_file = NULL;

/* what type of timestamp to put on logs */
static enum codeslogging_logstamp internal_logstamp = CODESLOGGING_LOGSTAMP_DEFAULT;

/*****************************************************************
 * prototypes
 */
static int codeslogging_disable_stderr(
    void);
static int codeslogging_disable_file(
    void);

static int codeslogging_debug_fp_va(
    FILE * fp,
    char prefix,
    const char *format,
    va_list ap,
    enum codeslogging_logstamp ts);

/*****************************************************************
 * visible functions
 */

/** Turns on logging to stderr.
 *
 *  \return 0 on success, -errno on failure.
 */
int codeslogging_enable_stderr(
    void)
{

    /* keep up with the existing logging settings */
    int tmp_debug_on = codeslogging_debug_on;
    uint64_t tmp_debug_mask = codeslogging_debug_mask;

    /* turn off any running facility */
    codeslogging_disable();

    codeslogging_facility = CODESLOGGING_STDERR;

    /* restore the logging settings */
    codeslogging_debug_on = tmp_debug_on;
    codeslogging_debug_mask = tmp_debug_mask;

    return 0;
}

/** Turns on logging to a file.  The filename argument indicates which
 *  file to use for logging messages, and the mode indicates whether the
 *  file should be truncated or appended (see fopen() man page).
 *
 *  \return 0 on success, -errno on failure.
 */
int codeslogging_enable_file(
    const char *filename,
    const char *mode)
{

    /* keep up with the existing logging settings */
    int tmp_debug_on = codeslogging_debug_on;
    uint64_t tmp_debug_mask = codeslogging_debug_mask;

    /* turn off any running facility */
    codeslogging_disable();

    internal_log_file = fopen(filename, mode);
    if(!internal_log_file)
    {
        return -errno;
    }

    codeslogging_facility = CODESLOGGING_FILE;

    /* restore the logging settings */
    codeslogging_debug_on = tmp_debug_on;
    codeslogging_debug_mask = tmp_debug_mask;

    return 0;
}

/** Turns off any active logging facility and disables debugging.
 *
 *  \return 0 on success, -errno on failure.
 */
int codeslogging_disable(
    void)
{
    int ret = -EINVAL;

    switch (codeslogging_facility)
    {
        case CODESLOGGING_STDERR:
            ret = codeslogging_disable_stderr();
            break;
        case CODESLOGGING_FILE:
            ret = codeslogging_disable_file();
            break;
        default:
            break;
    }

    codeslogging_debug_on = 0;
    codeslogging_debug_mask = 0;

    return ret;
}

/** Fills in args indicating whether debugging is on or off, and what the 
 *  mask level is.
 *
 *  \return 0 on success, -errno on failure.
 */
int codeslogging_get_debug_mask(
    int *debug_on,
    uint64_t * mask)
{
    *debug_on = codeslogging_debug_on;
    *mask = codeslogging_debug_mask;
    return 0;
}

/** Determines whether debugging messages are turned on or off.  Also
 *  specifies the mask that determines which debugging messages are
 *  printed.
 *
 *  \return 0 on success, -errno on failure.
 */
int codeslogging_set_debug_mask(
    int debug_on,
    uint64_t mask)
{
    if((debug_on != 0) && (debug_on != 1))
    {
        return -EINVAL;
    }

    codeslogging_debug_on = debug_on;
    codeslogging_debug_mask = mask;
    return 0;
}

/* codeslogging_set_logstamp()
 *
 * sets timestamp style for codeslogging messages
 *
 * returns 0 on success, -errno on failure
 */
int codeslogging_set_logstamp(
    enum codeslogging_logstamp ts)
{
    internal_logstamp = ts;
    return (0);
}

#ifndef __GNUC__
/* __codeslogging_debug_stub()
 * 
 * stub for codeslogging_debug that doesn't do anything; used when debugging
 * is "compiled out" on non-gcc builds
 *
 * returns 0
 */
int __codeslogging_debug_stub(
    uint64_t mask,
    char prefix,
    const char *format,
    ...)
{
    return 0;
}
#endif

/* __codeslogging_debug()
 * 
 * Logs a standard debugging message.  It will not be printed unless the
 * mask value matches (logical "and" operation) with the mask specified in
 * codeslogging_set_debug_mask() and debugging is turned on.
 *
 * returns 0 on success, -errno on failure
 */
int __codeslogging_debug(
    uint64_t mask,
    char prefix,
    const char *format,
    ...)
{
    int ret = -EINVAL;
    va_list ap;

    /* rip out the variable arguments */
    va_start(ap, format);
    ret = __codeslogging_debug_va(mask, prefix, format, ap);
    va_end(ap);

    return ret;
}

int __codeslogging_debug_va(
    uint64_t mask,
    char prefix,
    const char *format,
    va_list ap)
{
    int ret = -EINVAL;

    /* NOTE: this check happens in the macro (before making a function call)
     * if we use gcc 
     */
#ifndef __GNUC__
    /* exit quietly if we aren't meant to print */
    if((!codeslogging_debug_on) || !(codeslogging_debug_mask & mask) || (!codeslogging_facility))
    {
        return 0;
    }
#endif

    if(prefix == '?')
    {
        /* automatic prefix assignment */
        prefix = 'D';
    }

    switch (codeslogging_facility)
    {
        case CODESLOGGING_STDERR:
            ret = codeslogging_debug_fp_va(stderr, prefix, format, ap, internal_logstamp);
            break;
        case CODESLOGGING_FILE:
            ret =
                codeslogging_debug_fp_va(internal_log_file, prefix, format, ap, internal_logstamp);
            break;
        default:
            break;
    }

    return ret;
}

/** Logs a critical error message.  This will print regardless of the
 *  mask value and whether debugging is turned on or off, as long as some
 *  logging facility has been enabled.
 *
 *  \return 0 on success, -errno on failure.
 */
int codeslogging_err(
    const char *format,
    ...)
{
    va_list ap;
    int ret = -EINVAL;

    if(!codeslogging_facility)
    {
        return 0;
    }

    /* rip out the variable arguments */
    va_start(ap, format);

    switch (codeslogging_facility)
    {
        case CODESLOGGING_STDERR:
            ret = codeslogging_debug_fp_va(stderr, 'E', format, ap, internal_logstamp);
            break;
        case CODESLOGGING_FILE:
            ret = codeslogging_debug_fp_va(internal_log_file, 'E', format, ap, internal_logstamp);
            break;
        default:
            break;
    }

    va_end(ap);

    return ret;
}

#ifdef CODESLOGGING_ENABLE_BACKTRACE
#ifndef CODESLOGGING_BACKTRACE_DEPTH
#define CODESLOGGING_BACKTRACE_DEPTH 12
#endif
/** Prints out a dump of the current stack (excluding this function)
 *  using codeslogging_err.
 */
void codeslogging_backtrace(
    void)
{
    void *trace[CODESLOGGING_BACKTRACE_DEPTH];
    char **messages = NULL;
    int i, trace_size;

    trace_size = backtrace(trace, CODESLOGGING_BACKTRACE_DEPTH);
    messages = backtrace_symbols(trace, trace_size);
    for(i = 1; i < trace_size; i++)
    {
        codeslogging_err("\t[bt] %s\n", messages[i]);
    }
    free(messages);
}
#else
void codeslogging_backtrace(
    void)
{
}
#endif

/****************************************************************
 * Internal functions
 */

int codeslogging_debug_fp(
    FILE * fp,
    char prefix,
    enum codeslogging_logstamp ts,
    const char *format,
    ...)
{
    int ret;
    va_list ap;

    /* rip out the variable arguments */
    va_start(ap, format);
    ret = codeslogging_debug_fp_va(fp, prefix, format, ap, ts);
    va_end(ap);
    return ret;
}

/* codeslogging_debug_fp_va()
 * 
 * This is the standard debugging message function for the file logging
 * facility or to stderr.
 *
 * returns 0 on success, -errno on failure
 */
static int codeslogging_debug_fp_va(
    FILE * fp,
    char prefix,
    const char *format,
    va_list ap,
    enum codeslogging_logstamp ts)
{
    char buffer[CODESLOGGING_BUF_SIZE], *bptr = buffer;
    int bsize = sizeof(buffer);
    int ret = -EINVAL;
    struct timeval tv;
    time_t tp;

    sprintf(bptr, "[%c ", prefix);
    bptr += 3;
    bsize -= 3;

    switch (ts)
    {
        case CODESLOGGING_LOGSTAMP_USEC:
            gettimeofday(&tv, 0);
            tp = tv.tv_sec;
            strftime(bptr, 9, "%H:%M:%S", localtime(&tp));
            sprintf(bptr + 8, ".%06ld] ", (long) tv.tv_usec);
            bptr += 17;
            bsize -= 17;
            break;
        case CODESLOGGING_LOGSTAMP_DATETIME:
            gettimeofday(&tv, 0);
            tp = tv.tv_sec;
            strftime(bptr, 22, "%m/%d/%Y %H:%M:%S] ", localtime(&tp));
            bptr += 21;
            bsize -= 21;
            break;
        case CODESLOGGING_LOGSTAMP_THREAD:
            gettimeofday(&tv, 0);
            tp = tv.tv_sec;
            strftime(bptr, 9, "%H:%M:%S", localtime(&tp));
            sprintf(bptr + 8, ".%06ld (%ld)] ", (long) tv.tv_usec, pthread_self());
            bptr += 30;
            bsize -= 30;
            break;

        case CODESLOGGING_LOGSTAMP_NONE:
            bptr--;
            sprintf(bptr, "] ");
            bptr += 2;
            bsize++;
            break;
        default:
            break;
    }

    ret = vsnprintf(bptr, bsize, format, ap);
    if(ret < 0)
    {
        return -errno;
    }

    ret = fprintf(fp, "%s", buffer);
    if(ret < 0)
    {
        return -errno;
    }
    fflush(fp);

    return 0;
}

/* codeslogging_disable_stderr()
 * 
 * The shutdown function for the stderr logging facility.
 *
 * returns 0 on success, -errno on failure
 */
static int codeslogging_disable_stderr(
    void)
{
    /* this function doesn't need to do anything... */
    return 0;
}

/* codeslogging_disable_file()
 * 
 * The shutdown function for the file logging facility.
 *
 * returns 0 on success, -errno on failure
 */
static int codeslogging_disable_file(
    void)
{
    if(internal_log_file)
    {
        fclose(internal_log_file);
        internal_log_file = NULL;
    }
    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
