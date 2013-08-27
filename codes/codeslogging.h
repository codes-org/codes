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

/** \defgroup codeslogging codeslogging logging interface
 *
 * This is a basic application logging facility.  It uses printf style
 * formatting and provides several mechanisms for output.
 *
 * @{
 */

/* This code was derived from the PVFS2 codeslogging circa vs. 2.8.1 */

/** \file
 *
 *  Declarations for the codeslogging logging interface.
 */

#ifndef SRC_COMMON_LOGGING_CODESLOGGING_H
#define SRC_COMMON_LOGGING_CODESLOGGING_H

#include <stdint.h>
#include <stdarg.h>

/********************************************************************
 * Visible interface
 */

#define CODESLOGGING_BUF_SIZE 1024

/* what type of timestamp to place in msgs */
enum codeslogging_logstamp
{
    CODESLOGGING_LOGSTAMP_NONE = 0,
    CODESLOGGING_LOGSTAMP_USEC = 1,
    CODESLOGGING_LOGSTAMP_DATETIME = 2,
    CODESLOGGING_LOGSTAMP_THREAD = 3
};
#define CODESLOGGING_LOGSTAMP_DEFAULT CODESLOGGING_LOGSTAMP_USEC

/* stdio is needed by codeslogging_debug_fp declaration for FILE* */
#include <stdio.h>

int codeslogging_enable_stderr(
    void);
int codeslogging_enable_file(
    const char *filename,
    const char *mode);
int codeslogging_disable(
    void);
int codeslogging_set_debug_mask(
    int debug_on,
    uint64_t mask);
int codeslogging_get_debug_mask(
    int *debug_on,
    uint64_t * mask);
int codeslogging_set_logstamp(
    enum codeslogging_logstamp ts);

void codeslogging_backtrace(
    void);

#ifdef __GNUC__

/* do printf style type checking if built with gcc */
int __codeslogging_debug(
    uint64_t mask,
    char prefix,
    const char *format,
    ...) __attribute__ ((format(printf, 3, 4)));
int codeslogging_err(
    const char *format,
    ...) __attribute__ ((format(printf, 1, 2)));
int __codeslogging_debug_va(
    uint64_t mask,
    char prefix,
    const char *format,
    va_list ap);
int codeslogging_debug_fp(
    FILE * fp,
    char prefix,
    enum codeslogging_logstamp ts,
    const char *format,
    ...) __attribute__ ((format(printf, 4, 5)));

#ifdef CODESLOGGING_DISABLE_DEBUG
#define codeslogging_debug(mask, format, f...) do {} while(0)
#define codeslogging_perf_log(format, f...) do {} while(0)
#define codeslogging_debug_enabled(__m) 0
#else
extern int codeslogging_debug_on;
extern int codeslogging_facility;
extern uint64_t codeslogging_debug_mask;

#define codeslogging_debug_enabled(__m) \
    (codeslogging_debug_on && (codeslogging_debug_mask & __m))

/* try to avoid function call overhead by checking masks in macro */
#define codeslogging_debug(mask, format, f...)                  \
do {                                                      \
    if ((codeslogging_debug_on) && (codeslogging_debug_mask & mask) &&\
        (codeslogging_facility))                                \
    {                                                     \
        __codeslogging_debug(mask, '?', format, ##f);           \
    }                                                     \
} while(0)
#define codeslogging_perf_log(format, f...)                     \
do {                                                      \
    if ((codeslogging_debug_on) &&                              \
        (codeslogging_debug_mask & CODESLOGGING_PERFCOUNTER_DEBUG) && \
        (codeslogging_facility))                                \
    {                                                     \
        __codeslogging_debug(CODESLOGGING_PERFCOUNTER_DEBUG, 'P',     \
            format, ##f);                                 \
    }                                                     \
} while(0)

#endif /* CODESLOGGING_DISABLE_DEBUG */

/* do file and line number printouts w/ the GNU preprocessor */
#define codeslogging_ldebug(mask, format, f...)                  \
do {                                                       \
    codeslogging_debug(mask, "%s: " format, __func__ , ##f); \
} while(0)

#define codeslogging_lerr(format, f...)                  \
do {                                               \
    codeslogging_err("%s line %d: " format, __FILE__ , __LINE__ , ##f); \
    codeslogging_backtrace();                            \
} while(0)
#else /* ! __GNUC__ */

int __codeslogging_debug(
    uint64_t mask,
    char prefix,
    const char *format,
    ...);
int __codeslogging_debug_stub(
    uint64_t mask,
    char prefix,
    const char *format,
    ...);
int codeslogging_err(
    const char *format,
    ...);

#ifdef CODESLOGGING_DISABLE_DEBUG
#define codeslogging_debug(__m, __f, f...) __codeslogging_debug_stub(__m, '?', __f, ##f);
#define codeslogging_ldebug(__m, __f, f...) __codeslogging_debug_stub(__m, '?', __f, ##f);
#define codeslogging_debug_enabled(__m) 0
#else
#define codeslogging_debug(__m, __f, f...) __codeslogging_debug(__m, '?', __f, ##f);
#define codeslogging_ldebug(__m, __f, f...) __codeslogging_debug(__m, '?', __f, ##f);
#define codeslogging_debug_enabled(__m) \
            ((codeslogging_debug_on != 0) && (__m & codeslogging_debug_mask))

#endif /* CODESLOGGING_DISABLE_DEBUG */

#define codeslogging_lerr codeslogging_err

#endif /* __GNUC__ */

#endif /* __CODESLOGGING_H */

/* @} */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
