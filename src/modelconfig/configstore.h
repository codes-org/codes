/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SRC_COMMON_MODELCONFIG_CONFIGSTORE_H
#define SRC_COMMON_MODELCONFIG_CONFIGSTORE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Memory based storage for config file. 
 *
 * 
 */

/* error codes */
#define MCS_NOENT     -1   /* no such entry */
#define MCS_BUFSIZE   -2   /* buffer size too small */
#define MCS_WRONGTYPE -3   /* entry has wrong type */

struct mcs_entry;

typedef struct mcs_entry mcs_entry;


/* Return empty tree */
mcs_entry * mcs_initroot ();

/* Add child section to section; returns child section */
mcs_entry * mcs_addsection (mcs_entry * e, const char * name);

/* Add (multi-)key to section */
mcs_entry * mcs_addkey (mcs_entry * e, const char * name, const char **
      values, unsigned int count); 

/* Free subtree */
int mcs_freeroot (mcs_entry * e);

/* === examining tree === */

/* Returns true if entry is a subsection */
int mcs_issection (const mcs_entry * e);

/* Returns number of children in section */
int mcs_childcount (const mcs_entry * e); 

/* Return the number of values in the key */
int mcs_valuecount (const mcs_entry * e);

/* Retrieve the values for this key;
 * maxcount (input) contains the size of the buf array;
 * maxcount (output) the number of entries returned */
int mcs_getvaluemultiple (const mcs_entry * e, char ** buf, unsigned int * maxcount);

/* Retrieve single value for this key; Returns number of characters in key
 * value.  */
int mcs_getvaluesingle (const mcs_entry * e, char * buf, unsigned int
      bufsize);

/* Lookup the named subsection in section */
mcs_entry * mcs_findsubsection (const mcs_entry * e, const char * name);

/* Lookup the named key in section */
mcs_entry * mcs_findkey (const mcs_entry * e, const char * name);

/* Move to the next entry on this level */
mcs_entry * mcs_next (const mcs_entry * e);

/* Move to the first child of this section */
mcs_entry * mcs_child (const mcs_entry * e); 


typedef struct 
{
   char *       name;
   int          is_section;
   unsigned int childcount;
} mcs_section_entry;

/* returns number of entries written, or -1 on error */
int mcs_listsection (const mcs_entry * a, mcs_section_entry * e, unsigned int
      entries);

#ifdef __cplusplus
} /* extern "C" */
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
