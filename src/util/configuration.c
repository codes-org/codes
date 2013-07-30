#include <string.h>
#include "codes/configuration.h"
#include "codes/txt_configfile.h"

int configuration_load (const char *filepath, configuration_t *config)
{
    char *errstr;
    ConfigHandle ch;
    SectionHandle sh;
    SectionHandle subsh;
    SectionEntry se[10];
    SectionEntry subse[10];
    size_t se_count = 10;
    size_t subse_count = 10;
    int i, j, lpt;
    char data[256];

    memset (config, 0, sizeof(*config));
    errstr = NULL;

    ch = txtfile_openConfig(filepath, &errstr);
    cf_openSection(ch, ROOT_SECTION, "LPGROUPS", &sh);
    cf_listSection(ch, sh, se, &se_count); 

    for (i = 0; i < se_count; i++)
    {
        printf("section: %s type: %d\n", se[i].name, se[i].type);
        if (se[i].type == SE_SECTION)
        {
            subse_count = 10;
            cf_openSection(ch, sh, se[i].name, &subsh);
            cf_listSection(ch, subsh, subse, &subse_count);
            strncpy(config->lpgroups[i].name, se[i].name,
                    CONFIGURATION_MAX_NAME);
            config->lpgroups[i].repetitions = 1;
            config->lpgroups_count++;
            for (j = 0, lpt = 0; j < subse_count; j++)
            {
                if (subse[j].type == SE_KEY)
                {
                   cf_getKey(ch, subsh, subse[j].name, data, sizeof(data));
                   printf("key: %s value: %s\n", subse[j].name, data);
                   if (strcmp("repetitions", subse[j].name) == 0)
                   {
                       config->lpgroups[i].repetitions = atoi(data);
//		       printf("\n Repetitions: %d ", config->lpgroups[i].repetitions);
                   }
                   else
                   {
                       // assume these are lptypes and counts
                       strncpy(config->lpgroups[i].lptypes[lpt].name,
                               subse[j].name,
                               sizeof(config->lpgroups[i].lptypes[lpt].name));
                       config->lpgroups[i].lptypes[lpt].count = atoi(data);
                       config->lpgroups[i].lptypes_count++;
                       lpt++;
                   }
                }
            }
            cf_closeSection(ch, subsh);
        }
    }

    cf_closeSection(ch, sh);
    
    if (errstr) free(errstr);
    return 0;
}

int configuration_dump (configuration_t *config)
{
    int grp;
    int lpt;

    for (grp = 0; grp < config->lpgroups_count; grp++)
    {
        printf("group: %s\n", config->lpgroups[grp].name);
        printf("\trepetitions: %d\n", config->lpgroups[grp].repetitions);
        for (lpt = 0; lpt < config->lpgroups[grp].lptypes_count; lpt++)
        {
            printf("\t%s: %ld\n",
                   config->lpgroups[grp].lptypes[lpt].name,
                   config->lpgroups[grp].lptypes[lpt].count);
        }
    }

    return 0;
}
