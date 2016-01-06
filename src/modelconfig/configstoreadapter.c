/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "codes_base_config.h"
#include <assert.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <string.h>
#include "configstoreadapter.h"
#include "configstore.h"

/* unused attribute is only used here, so use directly in source */
#ifdef UNUSED
#elif defined(__GNUC__)
# define UNUSED(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
# define UNUSED(x) /*@unused@*/ x
#else
# define UNUSED(x) x
#endif /* UNUSED */

static int cfsa_getKey (void *  handle, SectionHandle section, const char * name,
      char * buf, size_t bufsize)
{
   mcs_entry * key = mcs_findkey ((mcs_entry*) (section ? section : handle), name);
   int count;
   char * tmp;
   unsigned int dcount;
   int ret = 1;

   if (!key)
      return -1;


   count = mcs_valuecount (key);
   
   if (count < 0)
      return count;

   /* error because key is multival */
   if (count > 1)
      return -2;

   /* if bufsize == 0 the user only wants to know the size and so we
    * ignore buf */
   if (bufsize == 0)
   {
      return mcs_getvaluesingle (key, 0, 0);
   }


   dcount = 1; 
   mcs_getvaluemultiple (key, &tmp, &dcount);

   if (!dcount)
   {
      assert(buf);
      *buf = 0;
      ret = 0;
      /* tmp was not modified no need to free */
   }
   else
   {
      ret = strlen (tmp);
      strncpy (buf, tmp, bufsize);
      if (bufsize > 0)
         buf[bufsize-1]=0;
      free (tmp);
   }
   return ret;
}

static int cfsa_getMultiKey (void * handle, SectionHandle section, const char *name,
      char *** buf, size_t * e)
{
   mcs_entry * key = mcs_findkey ((mcs_entry*) (section ? section : handle), 
         name);
   int count; 
   unsigned int dcount;

   *e = 0; 
   if (!key)
      return -1;

   count = mcs_valuecount (key);

   if (count < 0)
      return -2;

   *buf = (char **) malloc (sizeof (char **) * count);
   dcount = count;
   mcs_getvaluemultiple (key, *buf, &dcount);
   *e = dcount;

   return 1;
}

static int cfsa_listSection (void * handle, SectionHandle section, 
      SectionEntry * entries, size_t * maxentries)
{
   mcs_section_entry * out;
   int count;
   int ret = 0;
   int i;

   if (!section)
      section = handle;


   count = mcs_childcount ((mcs_entry *)section);
   if (count < 0)
   {
      *maxentries = 0;
      return count;
   }
   

   if (count == 0)
   {
      *maxentries = 0;
      return count;
   }

   ret = count;
   
   out = (mcs_section_entry*) malloc (sizeof(mcs_section_entry)* *maxentries);

   count = mcs_listsection ((mcs_entry*)section, (mcs_section_entry*)out,  *maxentries);
   if (count < 0)
   {
      *maxentries = 0;
      ret = count;
   }
   else
   {
      for (i=0; i<count; ++i)
      {
         entries[i].name = out[i].name;
         if (out[i].is_section)
         {
            entries[i].type = SE_SECTION;
         }
         else
         {
            entries[i].type = (out[i].childcount <= 1 ? SE_KEY : SE_MULTIKEY);
         }
      }
      *maxentries = count;
   }
   free (out);

   return ret;
}

static int cfsa_openSection (void * handle, SectionHandle section, const char *
      sectionname, SectionHandle * newsection)
{
   mcs_entry * e = mcs_findsubsection ((mcs_entry *) (section ? section : handle),
         sectionname);
   *newsection = e;
   return (e != 0 ? 1 : -1);
}

static int cfsa_closeSection (void * UNUSED(handle), SectionHandle UNUSED(section))
{
   return 1;
}

static int cfsa_createSection (void * handle, SectionHandle section, const
      char * name, SectionHandle * newsection)
{
   mcs_entry * news = mcs_addsection ( (mcs_entry*) (section ? section : handle),
         name);
   *newsection = news;
   return (news != 0);
}

static int cfsa_createKey (void * handle, SectionHandle section, const char * key,
      const char ** data, unsigned int count)
{
   return (mcs_addkey ((mcs_entry *) (section ? section : handle), key, data, count) != 0 ? 1 : -1);
}

static void cfsa_free (void * handle)
{
   mcs_freeroot ((mcs_entry*) handle);
}

static int cfsa_getSectionSize (void * handle, SectionHandle section,
      unsigned int * count)
{
   int c = mcs_childcount ((mcs_entry *) (section ? section : handle));
   if (c >= 0)
   {
      *count = c;
      return 1;
   }
   else
   {
      return c;
   }
}



static struct ConfigVTable cfsa_template = {
   .getKey = cfsa_getKey,
   .getMultiKey = cfsa_getMultiKey,
   .listSection = cfsa_listSection,
   .openSection = cfsa_openSection,
   .getSectionSize = cfsa_getSectionSize,
   .closeSection = cfsa_closeSection,
   .createSection = cfsa_createSection,
   .createKey = cfsa_createKey,
   .free = cfsa_free,
   .data = 0
};

struct ConfigVTable * cfsa_create (mcs_entry * e)
{
    struct ConfigVTable * newh = (struct ConfigVTable *) malloc (sizeof (struct ConfigVTable));
   *newh = cfsa_template;
   newh->data = e;
   return newh; 
}

struct ConfigVTable * cfsa_create_empty ()
{
   return cfsa_create (mcs_initroot ());
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
