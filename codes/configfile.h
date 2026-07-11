/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SRC_COMMON_MODELCONFIG_CONFIGFILE_H
#define SRC_COMMON_MODELCONFIG_CONFIGFILE_H

#include <stddef.h> /* size_t */
#include <stdio.h>  /* FILE (cf_equal_report) */
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * A config file is contains (possibly nested) sections, 
 * where each section groups entries. 
 * An entry is either another section, a key or a multikey.
 *
 * A key is a (name, value) string pair, while a multikey is a 
 * (name, value, value, value, ...) tuple.
 */
typedef void* SectionHandle;


enum { SE_SECTION = 1, SE_KEY = 2, SE_MULTIKEY = 3 };

typedef struct {
    char* name;
    unsigned int type;
} SectionEntry;

#define ROOT_SECTION ((SectionHandle)0)

struct ConfigVTable {
    /* File path of the configuration file. Used in computing the relative path
    * of file fields */
    char* config_dir;

    /* Returns number of characters in key or < 0 if an error occured 
    * (such as key is missing) 
    *
    * Calling this function with a NULL buf ptr and 0 bufsize will
    * return the keysize, not including terminating 0.
    *
    * */
    int (*getKey)(void* handle, SectionHandle section, const char* key, char* buf, size_t bufsize);

    /*
    * Reads list entries: after the function returns, buf will be set to 
    * a pointer pointing to an array of e char * pointers.
    * Needs to be freed with free() (array + pointers) 
    * Returns < 0 if error */
    int (*getMultiKey)(void* handle, SectionHandle section, const char* key, char*** buf,
                       size_t* e);
    /*
    * Outputs number of entries written in maxentries, retrieves at most
    * maxentries (input val).
    * Returns total number of entries in section. 
    */
    int (*listSection)(void* handle, SectionHandle section, SectionEntry* entries,
                       size_t* maxentries);

    /* Returns -1 if failed, >=0 if ok */
    int (*openSection)(void* handle, SectionHandle section, const char* sectionname,
                       SectionHandle* newsection);

    /* Return the number of entries in a section in count,
    * return code is <0 if error */
    int (*getSectionSize)(void* handle, SectionHandle section, unsigned int* count);

    /* Returns < 0 if error */
    int (*closeSection)(void* handle, SectionHandle section);

    /* Returns < 0 if error */
    int (*createSection)(void* handle, SectionHandle section, const char* name,
                         SectionHandle* newsection);

    /* Returns < 0 if error */
    int (*createKey)(void* handle, SectionHandle section, const char* key, const char** data,
                     unsigned int count);

    void (*free)(void* handle);

    void* data;
};

/* utility debug function: write config tree to stdout;
 * If all OK: ret >= 0, otherwise ret < 0 and *err is set
 * to error message
 * */
int cf_dump(struct ConfigVTable* cf, SectionHandle h, char** err);

/**
 * Compare two config trees for equality.
 *
 * Equality is structural and **order-insensitive**: the two trees are equal when
 * they hold the same named sections and keys with the same values, regardless of
 * the order entries were written -- because the config store keys every lookup by
 * name (sections case-insensitively, keys case-sensitively) and never by
 * position. The value *list* of a key is compared as an ordered tuple, so a
 * list-valued key such as `modelnet_order` must match value-for-value in order.
 *
 * @return 1 if the trees are equal, 0 otherwise.
 */
int cf_equal(struct ConfigVTable* h1, struct ConfigVTable* h2);

/**
 * Like cf_equal(), but on inequality writes a human-readable description of every
 * divergence -- each under its `section/key` path, saying whether an entry is
 * missing from one side, is a key on one side and a section on the other, or has
 * differing values -- to @p report. Pass a NULL @p report for the same result
 * with no output (identical to cf_equal). Intended for tests and diagnostics, so
 * a failed comparison points at the exact section/key that diverged rather than
 * only reporting that the trees differ.
 *
 * @param report  stream to write the divergence report to, or NULL for none.
 * @return 1 if the trees are equal, 0 otherwise.
 */
int cf_equal_report(struct ConfigVTable* h1, struct ConfigVTable* h2, FILE* report);

static inline int cf_free(struct ConfigVTable* cf) {
    if (!cf)
        return 1;

    cf->free(cf->data);
    free(cf);
    return 1;
}

static inline int cf_getSectionSize(struct ConfigVTable* cf, SectionHandle section,
                                    unsigned int* count) {
    return cf->getSectionSize(cf->data, section, count);
}

static inline int cf_closeSection(struct ConfigVTable* cf, SectionHandle section) {
    return cf->closeSection(cf->data, section);
}

static inline int cf_openSection(struct ConfigVTable* cf, SectionHandle section,
                                 const char* sectionname, SectionHandle* newsection) {
    return cf->openSection(cf->data, section, sectionname, newsection);
}

static inline int cf_getKey(struct ConfigVTable* cf, SectionHandle section, const char* keyname,
                            char* buf, size_t maxbuf) {
    return cf->getKey(cf->data, section, keyname, buf, maxbuf);
}

static inline int cf_getMultiKey(struct ConfigVTable* cf, SectionHandle section,
                                 const char* keyname, char*** buf, size_t* e) {
    return cf->getMultiKey(cf->data, section, keyname, buf, e);
}

static inline int cf_listSection(struct ConfigVTable* cf, SectionHandle section,
                                 SectionEntry* entries, size_t* maxentries) {
    return cf->listSection(cf->data, section, entries, maxentries);
}

static inline int cf_createSection(struct ConfigVTable* handle, SectionHandle section,
                                   const char* name, SectionHandle* newsection) {
    return handle->createSection(handle->data, section, name, newsection);
}

static inline int cf_createKey(struct ConfigVTable* handle, SectionHandle section, const char* key,
                               const char** data, unsigned int count) {
    return handle->createKey(handle->data, section, key, data, count);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
