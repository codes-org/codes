/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <stdio.h>
#include "txt_configfile.h"
#include "configglue.h"
#include "src/modelconfig/configparser.h"
#include "src/modelconfig/configlex.h"
#include "configstoreadapter.h"

#define MAX_CONFIG_SIZE 10*1024*1024

/* BISON doesn't declare the prototype? */
int cfgp_parse (yyscan_t * scanner, ParserParams * param);

static void show_indent (FILE * f, unsigned int ind)
{
   unsigned int i;

   for (i=0; i<ind; ++i)
      fprintf (f, " "); 
}

static inline int mymin (int v1, int v2)
{
   return (v1 < v2 ?  v1 : v2);
}

static int dump_section (FILE * f, struct ConfigVTable * h, SectionHandle s, unsigned
      int indent);

static int dump_entry (FILE * f, struct ConfigVTable * h, SectionHandle s, 
      unsigned int indent, const SectionEntry * entry)
{
   int ret = 1;
   switch (entry->type)
   {
      case SE_SECTION:
         {
            SectionHandle newsec;

            fprintf (f, "%s { \n", entry->name);

            ret = mymin(ret, cf_openSection (h, s, entry->name, &newsec));
            if (ret >= 0) 
            {
               ret = mymin (dump_section (f, h, newsec, indent + 2), ret);
               show_indent (f, indent);
               fprintf (f, "}\n");
               ret = mymin (ret, cf_closeSection (h, newsec));
            }
            break;
         }
      case SE_KEY:
         {
            char buf[255];
            ret = mymin (ret, cf_getKey (h, s, entry->name, &buf[0], sizeof(buf)));
            if (ret >= 0)
            {
                fprintf (f, "%s = \"%s\";\n", entry->name, buf); 
            }
            break;
         }
      case SE_MULTIKEY:
         {
            char ** ptrs;
            size_t size;
            size_t j;
            ret = mymin (ret, cf_getMultiKey (h, s, entry->name, &ptrs, &size));
            if (ret >= 0)
            {
               fprintf (f,"%s = (", entry->name);
               for (j=0; j<size; ++j)
               {
                  fprintf (f,"\"%s\" ", (ptrs[j] ? ptrs[j] : ""));
                  free (ptrs[j]);
                  if ((j+1) < size)
                     fprintf (f,", ");
               }
               fprintf (f, ");\n");
               free (ptrs);
            }
         }
   }
   return ret;
}

static int dump_section (FILE * f, struct ConfigVTable * h, SectionHandle s, unsigned
      int indent)
{
   unsigned int sectionsize;
   size_t count;
   unsigned int i;
   int ret = 1;
   SectionEntry * entries = 0;

   ret = mymin (ret, cf_getSectionSize (h, s, &sectionsize));
   if (ret < 0)
      return ret;

   count = sectionsize;
  
   entries = (SectionEntry*)malloc (sizeof (SectionEntry) * sectionsize);

   ret = mymin(ret, cf_listSection (h, s, &entries[0], &count));

   if (ret < 0)
      goto fail;

   assert(sectionsize == count);

   for (i=0; i<count; ++i)
   {
      show_indent (f,indent);
      if (ret >= 0)
      {
         ret = mymin (ret, dump_entry (f, h, s, indent, &entries[i]));
      }
      free ((void*)entries[i].name);
   }

fail:
   free (entries);
   return ret;
}


int txtfile_writeConfig (struct ConfigVTable * cf, SectionHandle h, FILE * f, char ** err)
{
   int ret;
   assert(err);
   assert(f);

   *err = 0;

   if ((ret = dump_section (f, cf, h, 0)) < 0)
   {
      *err = strdup ("Error accessing config tree!");
      return ret;
   }

   return ret;
}

struct ConfigVTable * txtfile_openStream (FILE * f, char ** err)
{
   long size;
   ParserParams p; 
   yyscan_t scanner;
   int reject;
   char buf[512];
   
   assert(err);

   *err=0;

   /* get file size */
   fseek (f, 0, SEEK_END);
 
   size = ftell (f);

   fseek (f, 0, SEEK_SET);
   if (size > MAX_CONFIG_SIZE)
   {
      *err = strdup ("Config file too large! Not parsing!");
      return 0;
   }

   cfgp_lex_init_extra (&p, &scanner);
   cfgp_set_in (f, scanner); 
   cfgp_initparams (&p, cfsa_create (mcs_initroot ()));
   reject = cfgp_parse((yyscan_t*)scanner, &p);
   cfgp_lex_destroy (scanner);

   /* either we have a valid confighandle or we have a parser error... */
   /* not true: we can have a partial config tree */
   // assert((p.error_code || p.configfile) && (!p.error_code || !p.configfile));

   /* If ther parser failed we need to have an error code */
   assert(!reject || p.parser_error_code || p.lexer_error_code);
   assert(!p.lexer_error_string || p.lexer_error_code);
   assert(!p.parser_error_string || p.parser_error_code);

   if (!cfgp_parse_ok (&p, buf, sizeof(buf)))
   {
      *err = strdup (buf);
   }
   else
   {
      assert(!p.parser_error_string);
      assert(!p.lexer_error_string);
      if (err) *err = 0;
   }

   cfgp_freeparams (&p);

   return p.configfile;
 }

struct ConfigVTable * txtfile_openConfig (const char * filename, char ** error)
{
   FILE  * f;
   struct ConfigVTable * ret;


   f = fopen (filename, "r");
   if (!f)
   {
      char buf[255];
      strerror_r (errno, buf, sizeof(buf));
      *error = strdup (buf);
      return 0;
   }

   ret = txtfile_openStream (f, error);

   fclose (f);

   return ret;
}


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
