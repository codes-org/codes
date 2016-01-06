/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SRC_COMMON_MODELCONFIG_TXTFILE_CONFIGFILE_H
#define SRC_COMMON_MODELCONFIG_TXTFILE_CONFIGFILE_H


#include <stdio.h>
#include "configfile.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ConfigFile implementation that stores data in a text file 
 */


/**
 * returns ConfigVTable, if all is OK *err is set to 0, 
 * otherwise *err is set to a pointer to the error string 
 * (which needs to be freed by the user)
 * NOTE that even if an error occurred, a partial ConfigVTable tree
 * can be returned.
 */
struct ConfigVTable * txtfile_openConfig (const char * filename, char ** err);

/**
 * returns ConfigVTable, if all is OK *err is set to 0, 
 * otherwise *err is set to a pointer to the error string 
 * (which needs to be freed by the user)
 * NOTE that even if an error occurred, a partial ConfigVTable tree
 * can be returned.
 */
struct ConfigVTable * txtfile_openStream (FILE * f, char ** err);


/**
 * Write ConfigVTable to disk (in a format supported by _open).
 * Returns >=0 if all went OK, < 0 otherwise in which 
 * case *err is set to a pointer to an error string, which needs to be
 * freed by the user.
 */
int txtfile_writeConfig (struct ConfigVTable * h, SectionHandle h2, FILE * out, char ** err);

#ifdef __cplusplus
} /* extern "C"  */
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
