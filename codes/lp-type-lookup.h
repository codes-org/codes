/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */


#ifndef LP_TYPE_LOOKUP_H
#define LP_TYPE_LOOKUP_H

#include "ross.h"

/* look up the lp type registered through lp_type_register. Mostly used
 * internally */
const tw_lptype* lp_type_lookup(const char* name);

/* register an LP with CODES/ROSS */
void lp_type_register(const char* name, const tw_lptype* type);

#endif /* LP_TYPE_LOOKUP_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
