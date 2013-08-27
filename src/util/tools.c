/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codes/tools.h"

char * safe_strncpy(char * buf, const char * source, unsigned int size)
{
    ALWAYS_ASSERT(buf);
    ALWAYS_ASSERT(source);
    ALWAYS_ASSERT(size);

    strncpy (buf, source, size);
    
    /* size will be >0 (assert above); */
    buf[size-1]=0;
    return buf;
}

void always_assert_error(const char * expr, const char * file, int lineno)
{
    fprintf(stderr, "Assertion '%s' failed (%s:%i)!\n", expr, file, lineno);
    abort();
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
