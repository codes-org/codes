/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <libgen.h>
#include "codes/configuration.h"

/*
 * Global to hold configuration in memory
 */
ConfigHandle config;

/* Global to hold LP configuration */
config_lpgroups_t lpconf;

int configuration_load (const char *filepath,
                        MPI_Comm comm,
                        ConfigHandle *handle)
{
    MPI_File   fh;
    MPI_Status status;
    MPI_Offset txtsize;
    FILE      *f;
    char      *txtdata;
    char      *error;
    int        rc;
    char      *tmp_path;

    rc = MPI_File_open(comm, (char*)filepath, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);
    assert(rc == MPI_SUCCESS);

    rc = MPI_File_get_size(fh, &txtsize);
    assert(rc == MPI_SUCCESS);

    txtdata = malloc(txtsize);
    assert(txtdata); 

    rc = MPI_File_read_all(fh, txtdata, txtsize, MPI_BYTE, &status);
    assert(rc == MPI_SUCCESS);

    rc = MPI_File_close(&fh);
    assert(rc == MPI_SUCCESS);

#ifdef __APPLE__
    f = fopen(filepath, "r");
#else
    f = fmemopen(txtdata, txtsize, "rb");
#endif
    assert(f);

    *handle = txtfile_openStream(f, &error);
    if (error)
    {
        fprintf(stderr, "config error: %s\n", error);
        free(error);
        rc = 1;
    }
    else
    {
        rc = 0;
    }

    fclose(f);

    /* NOTE: posix version overwrites argument :(. */
    tmp_path = strdup(filepath);
    (*handle)->config_dir = strdup(dirname(tmp_path));
    free(tmp_path);

    if (rc == 0)
        configuration_get_lpgroups(handle, "LPGROUPS", &lpconf);

    return rc;
}

int configuration_get_value(ConfigHandle *handle,
                            const char *section_name,
                            const char *key_name,
                            const char *annotation,
                            char *value,
                            size_t len)
{
    SectionHandle section_handle;
    int           rc;
    // reading directly from the config, so need to inject the annotation
    // directly into the search string
    char key_name_tmp[CONFIGURATION_MAX_NAME];
    char *key_name_full;
    if (annotation==NULL){
        // sorry const type... we promise we won't change you
        key_name_full = (char*) key_name;
    }
    else{
        assert(snprintf(key_name_tmp, CONFIGURATION_MAX_NAME, "%s@%s",
                    key_name, annotation) < CONFIGURATION_MAX_NAME);
        key_name_full = key_name_tmp;
    }

    rc = cf_openSection(*handle, ROOT_SECTION, section_name, &section_handle);
    assert(rc == 1);

    rc = cf_getKey(*handle, section_handle, key_name_full, value, len);
    assert(rc);

    (void) cf_closeSection(*handle, section_handle);

    return rc;
}

int configuration_get_value_relpath(
        ConfigHandle *handle,
        const char * section_name,
        const char * key_name,
        const char *annotation,
        char *value,
        size_t length){
    char *tmp = malloc(length);

    configuration_get_value(handle, section_name, key_name, annotation, tmp,
            length);

    /* concat the configuration value with the directory */
    int w = snprintf(value, length, "%s/%s", (*handle)->config_dir, tmp);

    free(tmp);
    return w;
}

int configuration_get_multivalue(ConfigHandle *handle,
                                 const char *section_name,
                                 const char *key_name,
                                 const char *annotation,
                                 char ***values,
                                 size_t *len)
{
    SectionHandle section_handle;
    int           rc;
    // reading directly from the config, so need to inject the annotation
    // directly into the search string
    char key_name_tmp[CONFIGURATION_MAX_NAME];
    char *key_name_full;
    if (annotation==NULL){
        // sorry const type... we promise we won't change you
        key_name_full = (char*) key_name;
    }
    else{
        assert(snprintf(key_name_tmp, CONFIGURATION_MAX_NAME, "%s@%s",
                    key_name, annotation) < CONFIGURATION_MAX_NAME);
        key_name_full = key_name_tmp;
    }

    rc = cf_openSection(*handle, ROOT_SECTION, section_name, &section_handle);
    assert(rc == 1);

    rc = cf_getMultiKey(*handle, section_handle, key_name_full, values, len);
    assert(rc);

    (void) cf_closeSection(*handle, section_handle);

    return rc;
}

int configuration_get_value_int (ConfigHandle *handle,
                                 const char *section_name,
                                 const char *key_name,
                                 const char *annotation,
                                 int *value)
{
    char valuestr[256];
    int rc = 1;
    int r;

    r = configuration_get_value(handle,
                                section_name,
                                key_name,
                                annotation,
                                valuestr,
                                sizeof(valuestr));
    if (r > 0)
    {
        *value = atoi(valuestr);
        rc = 0;
    }

    return rc;
}

int configuration_get_value_uint (ConfigHandle *handle,
                                  const char *section_name,
                                  const char *key_name,
                                  const char *annotation,
                                  unsigned int *value)
{
    char valuestr[256];
    int rc = 1;
    int r;

    r = configuration_get_value(handle,
                                section_name,
                                key_name,
                                annotation,
                                valuestr,
                                sizeof(valuestr));
    if (r > 0)
    {
        *value = (unsigned int) atoi(valuestr);
        rc = 0;
    }

    return rc;
}

int configuration_get_value_longint (ConfigHandle *handle,
                                     const char *section_name,
                                     const char *key_name,
                                     const char *annotation,
                                     long int *value)
{
    char valuestr[256];
    int rc = 1;
    int r;

    r = configuration_get_value(handle,
                                section_name,
                                key_name,
                                annotation,
                                valuestr,
                                sizeof(valuestr));
    if (r > 0)
    {
        errno = 0;
        *value = strtol(valuestr, NULL, 10);
        rc = errno;
    }

    return rc;
}

int configuration_get_value_double (ConfigHandle *handle,
                                    const char *section_name,
                                    const char *key_name,
                                    const char *annotation,
                                    double *value)
{
    char valuestr[256];
    int rc = 1;
    int r;

    r = configuration_get_value(handle,
                                section_name,
                                key_name,
                                annotation,
                                valuestr,
                                sizeof(valuestr));
    if (r > 0)
    {
        errno = 0;
        *value = strtod(valuestr, NULL);
        rc = errno;
    }

    return rc;
}

static void check_add_anno(
        const char *anno,
        config_anno_map_t *map){
    if (anno[0] == '\0'){
        map->has_unanno_lp = 1;
    }
    else{
        uint64_t a = 0;
        for (; a < map->num_annos; a++){
            if (strcmp(map->annotations[a], anno) == 0){
                map->num_anno_lps[a]++;
                break;
            }
        }
        if (a == map->num_annos){
            // we have a new anno!
            assert(a < CONFIGURATION_MAX_ANNOS);
            map->annotations[a] = strdup(anno);
            map->num_annos++;
            map->num_anno_lps[a] = 1;
        } // else anno was already there, do nothing
    }
}
static void check_add_lp_type_anno(
        const char *lp_name,
        const char *anno,
        config_lpgroups_t *lpgroups){
    uint64_t lpt_anno = 0;
    for (; lpt_anno < lpgroups->lpannos_count; lpt_anno++){
        config_anno_map_t *map = &lpgroups->lpannos[lpt_anno];
        if (strcmp(map->lp_name, lp_name) == 0){
            check_add_anno(anno, map);
        }
    }
    if (lpt_anno == lpgroups->lpannos_count){
        // we haven't seen this lp type before
        assert(lpt_anno < CONFIGURATION_MAX_TYPES);
        config_anno_map_t *map = &lpgroups->lpannos[lpt_anno];
        // initialize this annotation map
        strcpy(map->lp_name, lp_name);
        map->num_annos = 0;
        map->has_unanno_lp = 0;
        memset(map->num_anno_lps, 0, 
                CONFIGURATION_MAX_ANNOS*sizeof(*map->num_anno_lps));
        check_add_anno(anno, map);
        lpgroups->lpannos_count++;
    }
}

int configuration_get_lpgroups (ConfigHandle *handle,
                                const char *section_name,
                                config_lpgroups_t *lpgroups)
{
    SectionHandle sh;
    SectionHandle subsh;
    SectionEntry se[10];
    SectionEntry subse[10];
    size_t se_count = 10;
    size_t subse_count = 10;
    int i, j, lpt;
    char data[256];

    memset (lpgroups, 0, sizeof(*lpgroups));

    cf_openSection(*handle, ROOT_SECTION, section_name, &sh);
    cf_listSection(*handle, sh, se, &se_count); 

    for (i = 0; i < se_count; i++)
    {
        //printf("section: %s type: %d\n", se[i].name, se[i].type);
        if (se[i].type == SE_SECTION)
        {
            subse_count = 10;
            cf_openSection(*handle, sh, se[i].name, &subsh);
            cf_listSection(*handle, subsh, subse, &subse_count);
            strncpy(lpgroups->lpgroups[i].name, se[i].name,
                    CONFIGURATION_MAX_NAME);
            lpgroups->lpgroups[i].repetitions = 1;
            lpgroups->lpgroups_count++;
            for (j = 0, lpt = 0; j < subse_count; j++)
            {
                if (subse[j].type == SE_KEY)
                {
                   cf_getKey(*handle, subsh, subse[j].name, data, sizeof(data));
                   //printf("key: %s value: %s\n", subse[j].name, data);
                   if (strcmp("repetitions", subse[j].name) == 0)
                   {
                       lpgroups->lpgroups[i].repetitions = atoi(data);
		       //printf("\n Repetitions: %ld ", lpgroups->lpgroups[i].repetitions);
                   }
                   else
                   {
                       size_t s = sizeof(lpgroups->lpgroups[i].lptypes[lpt].name);
                       char *nm   = lpgroups->lpgroups[i].lptypes[lpt].name;
                       char *anno = lpgroups->lpgroups[i].lptypes[lpt].anno;
                       // assume these are lptypes and counts
                       strncpy(nm, subse[j].name, s-1);
                       lpgroups->lpgroups[i].lptypes[lpt].name[s-1] = '\0';

                       char *c = strchr(nm, '@');
                       if (c) {
                           strcpy(anno, c+1);
                           *c = '\0';
                       }
                       else {
                           anno[0] = '\0';
                       }
                       // add to anno map
                       check_add_lp_type_anno(nm, anno, lpgroups);
                       lpgroups->lpgroups[i].lptypes[lpt].count = atoi(data);
                       lpgroups->lpgroups[i].lptypes_count++;
                       lpt++;
                   }
                }
            }
            cf_closeSection(*handle, subsh);
        }
    }

    cf_closeSection(*handle, sh);
    
    return 0;
}

/*
 * Helper function - get the position in the LP annotation list of the
 * given annotation. Used for configuration schemes where an array of
 * configuration values is generated based on the annotations in
 * config_anno_map_t
 * If anno is not found or a NULL anno is passed in,
 * -1 is returned */
int configuration_get_annotation_index(const char *              anno,
                                       const config_anno_map_t * anno_map){
    if (anno == NULL) return -1;
    for (uint64_t i = 0; i < anno_map->num_annos; i++){
        if (!strcmp(anno, anno_map->annotations[i])){
            return (int)i;
        }
    }
    return -1;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
