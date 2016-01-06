/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "codes_base_config.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include "configfile.h"
#include "txt_configfile.h"

static int cf_equal_helper (struct ConfigVTable * h1, SectionHandle s1, struct ConfigVTable * h2, 
      SectionHandle s2)
{
   unsigned int sectionsize1;
   unsigned int sectionsize2;
   size_t count1;
   size_t count2;
   unsigned int i;
   int ret = 1;

   cf_getSectionSize (h1, s1, &sectionsize1);
   cf_getSectionSize (h2, s2, &sectionsize2);

   count1 = sectionsize1;
   count2 = sectionsize2;

   if (count1 != count2)
      return 0;
  
   SectionEntry entries1[sectionsize1];
   SectionEntry entries2[sectionsize2];

   cf_listSection (h1, s1, &entries1[0], &count1);
   cf_listSection (h2, s2, &entries2[0], &count2);

   for (i=0; i<count1; ++i)
   {
      if (entries1[i].type == entries2[i].type)
      {
         switch (entries1[i].type)
         {
            case SE_SECTION:
               {
                  SectionHandle newsec1;
                  SectionHandle newsec2;
                  int corresponding = -1;
                  int j;

                  /* find matching section in 2nd tree */
                  j=0;
                  while ((int)j < (int)count2)
                  {
                     if (!strcmp (entries1[i].name, entries2[j].name))
                     {
                        corresponding = j;
                        break;
                     }
                     ++j;
                  }

                  if (corresponding < 0)
                  {
                     /* missing section in 2nd tree */
                     ret=0;
                     break;
                  }

                  if (entries1[i].type != entries2[j].type)
                  {
                     ret=0;  /* name exists but is of different type */
                     break;
                  }

                  cf_openSection (h1, s1, entries1[i].name, &newsec1);
                  cf_openSection (h2, s2, entries2[j].name, &newsec2);

                  ret = cf_equal_helper (h1, newsec1, h2, newsec2);

                  cf_closeSection (h1, newsec1);
                  cf_closeSection (h2, newsec2);

                  break;
               }
            case SE_KEY:
               {
                  char buf1[255];
                  char buf2[255];
                  buf1[0] = buf2[0] = 0;

                  if (cf_getKey (h1, s1, entries1[i].name, &buf1[0], sizeof(buf1))<=0)
                  {
                     ret=0;break;
                  }
                  if (cf_getKey (h2, s2, entries1[i].name, &buf2[0], sizeof(buf2))<=0)
                  {
                     ret=0; break;
                  }

                  if (strcmp (buf1, buf2))
                  {
                     ret = 0;  /* strings not equal! */
                  }

                  break;
               }
            case SE_MULTIKEY:
               {
                  char ** ptrs1;
                  size_t size1 =0;
                  char ** ptrs2;
                  size_t size2 =0;
                  size_t j;

                  do
                  {
                     if (cf_getMultiKey (h1, s1, entries1[i].name, &ptrs1, &size1)<=0)
                     { ret=0; break; }
                     if (cf_getMultiKey (h2, s2, entries1[i].name, &ptrs2, &size2)<=0)
                     { ret=0; break; }

                     if (size1 != size2)
                     { ret=0; break; }

                     for (j=0; j<size1; ++j)
                     {
                        if (strcmp (ptrs1[j], ptrs2[j]))
                        {
                           ret=0; break;
                        }
                     }
                  } while (0);

                  for (j=0; j<size1; ++j)
                     free (ptrs1[j]);
                  for (j=0; j<size2; ++j)
                     free (ptrs2[j]);

                  free (ptrs1);
                  free (ptrs2);
               }

         }
      }
      else
      {
         ret=0;
      }
      if (!ret)
         break;
   }

   /* cleanup */
   for (i=0; i<count1; ++i)
   {
      free ((char*)entries1[i].name);
      free ((char*)entries2[i].name);
   }

   return ret;
}

int cf_equal (struct ConfigVTable * h1, struct ConfigVTable * h2)
{
   return cf_equal_helper (h1, ROOT_SECTION, h2, ROOT_SECTION);
}


int cf_dump (struct ConfigVTable * cf, SectionHandle h, char ** err)
{
   return txtfile_writeConfig (cf, h, stdout, err);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
