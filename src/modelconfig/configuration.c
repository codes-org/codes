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
#include <ross.h>

#include <codes/configfile.h>
#include "txt_configfile.h"

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
    FILE      *f = NULL;
    char      *txtdata = NULL;
    char      *error = NULL;
    int        rc = 0;
    char      *tmp_path = NULL;

    rc = MPI_File_open(comm, (char*)filepath, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);
    if (rc != MPI_SUCCESS) goto finalize;

    rc = MPI_File_get_size(fh, &txtsize);
    if (rc != MPI_SUCCESS) goto finalize;

    txtdata = (char*) malloc(txtsize);
    assert(txtdata);

    rc = MPI_File_read_all(fh, txtdata, txtsize, MPI_BYTE, &status);
    if (rc != MPI_SUCCESS) goto finalize;

#ifdef __APPLE__
    f = fopen(filepath, "r");
#else
    f = fmemopen(txtdata, txtsize, "rb");
#endif
    if (!f) { rc = 1; goto finalize; }

    *handle = txtfile_openStream(f, &error);
    if (error) { rc = 1; goto finalize; }

    /* NOTE: posix version overwrites argument :(. */
    tmp_path = strdup(filepath);
    assert(tmp_path);
    (*handle)->config_dir = strdup(dirname(tmp_path));
    assert((*handle)->config_dir);

    rc = configuration_get_lpgroups(handle, "LPGROUPS", &lpconf);

finalize:
    if (fh != MPI_FILE_NULL) MPI_File_close(&fh);
    if (f) fclose(f);
    free(txtdata);
    free(tmp_path);
    if (error) {
        fprintf(stderr, "config error: %s\n", error);
        free(error);
    }

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
        if (snprintf(key_name_tmp, CONFIGURATION_MAX_NAME, "%s@%s",
                    key_name, annotation) >= CONFIGURATION_MAX_NAME) {
            fprintf(stderr,
                    "config error: name@annotation pair too long: %s@%s\n",
                    key_name, annotation);
            return 1;
        }
        else
            key_name_full = key_name_tmp;
    }

    rc = cf_openSection(*handle, ROOT_SECTION, section_name, &section_handle);
    if (rc != 1) return 0;

    rc = cf_getKey(*handle, section_handle, key_name_full, value, len);
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
    char *tmp = (char*) malloc(length);

    int w = configuration_get_value(handle, section_name, key_name, annotation, tmp,
            length);
    if (w <= 0)
        return w;

    /* concat the configuration value with the directory */
    w = snprintf(value, length, "%s/%s", (*handle)->config_dir, tmp);

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
        if (snprintf(key_name_tmp, CONFIGURATION_MAX_NAME, "%s@%s",
                    key_name, annotation) >= CONFIGURATION_MAX_NAME) {
            fprintf(stderr,
                    "config error: name@annotation pair too long: %s@%s\n",
                    key_name, annotation);
            return 1;
        }
        else
            key_name_full = key_name_tmp;
    }

    rc = cf_openSection(*handle, ROOT_SECTION, section_name, &section_handle);
    if (rc != 1) return rc;

    rc = cf_getMultiKey(*handle, section_handle, key_name_full, values, len);
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
        int anno_offset,
        config_anno_map_t *map){
    if (anno_offset == -1) {
        map->has_unanno_lp = 1;
        if (!map->num_annos)
            map->is_unanno_first = 1;
    }
    else{
        int a = 0;
        for (; a < map->num_annos; a++){
            if (map->annotations[a].offset == anno_offset) {
                map->num_anno_lps[a]++;
                break;
            }
        }
        if (a == map->num_annos){
            // we have a new anno!
            assert(a < CONFIGURATION_MAX_ANNOS);
            map->annotations[a].offset = anno_offset;
            map->num_annos++;
            map->num_anno_lps[a] = 1;
        } // else anno was already there, do nothing
    }
}
static void check_add_lp_type_anno(
        int lp_name_offset,
        int anno_offset,
        config_lpgroups_t *lpgroups){
    int lpt_anno = 0;
    for (; lpt_anno < lpgroups->lpannos_count; lpt_anno++){
        config_anno_map_t *map = &lpgroups->lpannos[lpt_anno];
        if (map->lp_name.offset == lp_name_offset){
            check_add_anno(anno_offset, map);
            break;
        }
    }
    if (lpt_anno == lpgroups->lpannos_count){
        // we haven't seen this lp type before
        assert(lpt_anno < CONFIGURATION_MAX_TYPES);
        config_anno_map_t *map = &lpgroups->lpannos[lpt_anno];
        // initialize this annotation map
        map->lp_name.offset = lp_name_offset;
        map->num_annos = 0;
        map->has_unanno_lp = 0;
        map->is_unanno_first = 0;
        memset(map->num_anno_lps, 0, 
                CONFIGURATION_MAX_ANNOS*sizeof(*map->num_anno_lps));
        check_add_anno(anno_offset, map);
        lpgroups->lpannos_count++;
    }
}

#define REALLOC_IF(_cap_var, _len_exp, _buf_var) \
    do { \
        while ((_cap_var) <= (_len_exp)) { \
            _cap_var *= 2; \
            _buf_var = realloc(_buf_var, (_cap_var) * sizeof(*_buf_var)); \
            assert(_buf_var); \
        } \
    } while (0)

/* helper for setting up the canonical name mapping */
static int check_add_uniq_str(
        int ** offset_array,
        char ** str_buf,
        int * num_strs,
        int * offset_array_cap, // buffer capacity for resizing
        int * str_buf_len,
        int * str_buf_cap,   // buffer capacity for resizing
        char const * str)
{
    int slen = strlen(str);

    for (int i = 0; i < *num_strs; i++) {
        char const * b = (*str_buf) + (*offset_array)[i];
        int blen = strlen(b);
        if (slen == blen && memcmp(b, str, blen) == 0)
            return i;
    }

    REALLOC_IF(*offset_array_cap, *num_strs, *offset_array);
    REALLOC_IF(*str_buf_cap, *str_buf_len + slen + 1, *str_buf);

    // include null char
    memcpy(*str_buf + *str_buf_len, str, slen+1);
    (*offset_array)[*num_strs] = *str_buf_len;
    *num_strs += 1;
    *str_buf_len += slen+1;
    return *num_strs-1;
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
    char data[256];
    // buffer mgmt vars
    int num_uniq_group_names = 0;
    int group_names_buf_len = 0;
    int lp_names_buf_len = 0;
    int anno_names_buf_len = 0;
    int group_names_cap = 1;
    int lp_names_cap = 1;
    int anno_names_cap = 1;
    int group_names_buf_cap = 1;
    int lp_names_buf_cap = 1;
    int anno_names_buf_cap = 1;
    int name_pos;

    memset (lpgroups, 0, sizeof(*lpgroups));

    int *group_names_offsets =
        malloc(sizeof(*group_names_offsets) * group_names_cap);
    int *lp_names_offsets =
        malloc(sizeof(*lp_names_offsets) * lp_names_cap);
    int *anno_names_offsets =
        malloc(sizeof(*anno_names_offsets) * anno_names_cap);
    lpgroups->group_names_buf =
        malloc(sizeof(*lpgroups->group_names_buf) * group_names_buf_cap);
    lpgroups->lp_names_buf =
        malloc(sizeof(*lpgroups->lp_names_buf) * lp_names_buf_cap);
    lpgroups->anno_names_buf =
        malloc(sizeof(*lpgroups->anno_names_buf) * anno_names_buf_cap);
    assert(group_names_offsets != NULL);
    assert(lp_names_offsets != NULL);
    assert(anno_names_offsets != NULL);
    assert(lpgroups->group_names_buf != NULL);
    assert(lpgroups->lp_names_buf != NULL);
    assert(lpgroups->anno_names_buf != NULL);

    int ret = cf_openSection(*handle, ROOT_SECTION, section_name, &sh);
    if (ret == -1)
        return -1;
    cf_listSection(*handle, sh, se, &se_count); 

#define CHECKED_STRTOL(_val, _field, _data) \
    do{ \
        errno = 0; \
        long int _rd = strtol(_data, NULL, 10); \
        if (_rd <= 0 || errno) \
            tw_error(TW_LOC, "bad value (expected positive integer) for " \
                    "\"%s\": %s\n", _field, _data); \
        else \
            _val = _rd; \
    }while(0);

    for (size_t i = 0; i < se_count; i++)
    {
        //printf("section: %s type: %d\n", se[i].name, se[i].type);
        if (se[i].type == SE_SECTION)
        {
            subse_count = 10;
            cf_openSection(*handle, sh, se[i].name, &subsh);
            cf_listSection(*handle, subsh, subse, &subse_count);
            name_pos = check_add_uniq_str(&group_names_offsets,
                    &lpgroups->group_names_buf, &num_uniq_group_names,
                    &group_names_cap, &group_names_buf_len,
                    &group_names_buf_cap, se[i].name);
            lpgroups->lpgroups[i].name.offset = group_names_offsets[name_pos];
            lpgroups->lpgroups[i].repetitions = 1;
            lpgroups->lpgroups_count++;
            if (num_uniq_group_names != lpgroups->lpgroups_count)
                tw_error(TW_LOC,
                        "config error: non-unique group names detected\n");

            for (size_t j = 0, lpt = 0; j < subse_count; j++)
            {
                if (subse[j].type == SE_KEY)
                {
                   cf_getKey(*handle, subsh, subse[j].name, data, sizeof(data));
                   //printf("key: %s value: %s\n", subse[j].name, data);
                   if (strcmp("repetitions", subse[j].name) == 0)
                   {
                       CHECKED_STRTOL(lpgroups->lpgroups[i].repetitions,
                               "repetitions", data);
		       //printf("\n Repetitions: %ld ", lpgroups->lpgroups[i].repetitions);
                   }
                   else
                   {
                       // to avoid copy, find a possible annotation, change the
                       // string in the config structure itself, then change
                       // back for posterity
                       char *c = strchr(subse[j].name, '@');
                       if (c != NULL) {
                           *c = '\0';
                           name_pos = check_add_uniq_str(
                                   &anno_names_offsets,
                                   &lpgroups->anno_names_buf,
                                   &lpgroups->num_uniq_annos,
                                   &anno_names_cap,
                                   &anno_names_buf_len,
                                   &anno_names_buf_cap,
                                   c+1);
                           lpgroups->lpgroups[i].lptypes[lpt].anno.offset =
                                   anno_names_offsets[name_pos];
                       }
                       else
                           lpgroups->lpgroups[i].lptypes[lpt].anno.offset = -1;

                       name_pos = check_add_uniq_str(
                               &lp_names_offsets,
                               &lpgroups->lp_names_buf,
                               &lpgroups->num_uniq_lptypes,
                               &lp_names_cap,
                               &lp_names_buf_len,
                               &lp_names_buf_cap,
                               subse[j].name);
                       lpgroups->lpgroups[i].lptypes[lpt].name.offset =
                               lp_names_offsets[name_pos];

                       if (c != NULL)
                           *c = '@';

                       // add to anno map
                       check_add_lp_type_anno(
                               lpgroups->lpgroups[i].lptypes[lpt].name.offset,
                               lpgroups->lpgroups[i].lptypes[lpt].anno.offset,
                               lpgroups);
                       CHECKED_STRTOL(lpgroups->lpgroups[i].lptypes[lpt].count,
                               lpgroups->lpgroups[i].lptypes[lpt].name, data);
                       lpgroups->lpgroups[i].lptypes_count++;
                       lpt++;
                   }
                }
            }
            cf_closeSection(*handle, subsh);
        }
    }

    // set up the string pointers
    lpgroups->group_names =
        malloc(lpgroups->lpgroups_count * sizeof(*lpgroups->group_names));
    lpgroups->lp_names =
        malloc(lpgroups->num_uniq_lptypes * sizeof(*lpgroups->lp_names));
    assert(lpgroups->group_names);
    assert(lpgroups->lp_names);

    for (int i = 0; i < lpgroups->lpgroups_count; i++) {
        lpgroups->group_names[i] = lpgroups->group_names_buf +
            group_names_offsets[i];
    }
    for (int i = 0; i < lpgroups->num_uniq_lptypes; i++) {
        lpgroups->lp_names[i] = lpgroups->lp_names_buf +
            lp_names_offsets[i];
    }
    if (lpgroups->num_uniq_annos == 0) {
        free(lpgroups->anno_names_buf);
        lpgroups->anno_names = NULL;
        lpgroups->anno_names_buf = NULL;
    }
    else {
        lpgroups->anno_names =
            malloc(lpgroups->num_uniq_annos * sizeof(*lpgroups->anno_names));
        assert(lpgroups->anno_names);
        for (int i = 0; i < lpgroups->num_uniq_annos; i++) {
            lpgroups->anno_names[i] = lpgroups->anno_names_buf +
                anno_names_offsets[i];
        }
    }

    // everything is set up in offset mode, now make a second pass and convert
    // to pointers
    for (int g = 0; g < lpgroups->lpgroups_count; g++) {
        config_lpgroup_t *lpg = &lpgroups->lpgroups[g];
        lpg->name.ptr = lpgroups->group_names_buf + lpg->name.offset;
        for (int t = 0; t < lpg->lptypes_count; t++) {
            config_lptype_t *lpt = &lpg->lptypes[t];
            lpt->name.ptr = lpgroups->lp_names_buf + lpt->name.offset;
            lpt->anno.ptr =
                (lpt->anno.offset == -1)
                    ? NULL
                    : lpgroups->anno_names_buf + lpt->anno.offset;
        }
    }
    for (int m = 0; m < lpgroups->lpannos_count; m++) {
        config_anno_map_t *map = &lpgroups->lpannos[m];
        map->lp_name.ptr = lpgroups->lp_names_buf + map->lp_name.offset;
        for (int a = 0; a < map->num_annos; a++) {
            map->annotations[a].ptr =
                (map->annotations[a].offset == -1)
                    ? NULL
                    : lpgroups->anno_names_buf + map->annotations[a].offset;
        }
    }

    cf_closeSection(*handle, sh);

    free(group_names_offsets);
    free(lp_names_offsets);
    free(anno_names_offsets);

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
    for (int i = 0; i < anno_map->num_annos; i++){
        if (!strcmp(anno, anno_map->annotations[i].ptr)){
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
