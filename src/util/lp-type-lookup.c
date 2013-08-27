/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <string.h>
#include <assert.h>

#include "ross.h"
#include "codes/lp-type-lookup.h"

#define MAX_LP_TYPES 64

struct lp_name_mapping
{
    const char* name;
    const tw_lptype* type;
};

static struct lp_name_mapping map_array[MAX_LP_TYPES];
static int map_array_size = 0;

void lp_type_register(const char* name, const tw_lptype* type)
{
    map_array[map_array_size].name = strdup(name);
    assert(map_array[map_array_size].name);
    map_array[map_array_size].type = type;
    map_array_size++;

    return;
}

const tw_lptype* lp_type_lookup(const char* name)
{
    int i;

    for(i=0; i<map_array_size; i++)
    {
        if(strcmp(name, map_array[i].name) == 0)
        {
            return(map_array[i].type);
        }
    }

    return(NULL);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
