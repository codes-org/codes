/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SRC_COMMON_UTIL_TOOLS_H
#define SRC_COMMON_UTIL_TOOLS_H

#ifdef UNUSED
#elif defined(__GNUC__)
# define UNUSED(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
# define UNUSED(x) /*@unused@*/ x
#else
# define UNUSED(x) x
#endif /* UNUSED */

#define codesmin(a,b) ((a)<(b) ? (a):(b))
#define codesmax(a,b) ((a)>(b) ? (a):(b))

void always_assert_error(const char * expr, const char * file, int lineno);

#define ALWAYS_ASSERT(a) if (!(a)) always_assert_error(#a,__FILE__, __LINE__);

#define ARRAY_SIZEOF(a) (sizeof(a)/sizeof(a[0]))

char * safe_strncpy(char * buf, const char * source, unsigned int bufsize);

#define CODES_FLAG_ISSET(mode_, flag_) ((mode_ & flag_) != 0)
#define CODES_FLAG_SET(mode_, flag_, branch_)   \
do                                              \
{                                               \
    mode_ = mode_ | flag_;                      \
    branch_ = 1;                                \
}while(0)

#define CODES_FLAG_SET_RC(mode_, flag_, branch_)   \
do                                                  \
{                                                   \
    if(branch_)                                     \
    {                                               \
        mode_ = mode_ & (~flag_);                   \
    }                                               \
}while(0)

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
