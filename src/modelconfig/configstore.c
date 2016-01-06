/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "codes_base_config.h"
#include <string.h>
#include <assert.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include "configstore.h"


struct  mcs_entry
{
   const char * name;
   unsigned int is_section;
   struct mcs_entry * next;
   union
   {
      const char        * value;
      struct mcs_entry  * child;
   };
};


static inline mcs_entry * mcs_allocentry ()
{
    mcs_entry * n = (mcs_entry *) malloc (sizeof (mcs_entry));
   memset (n, 0, sizeof (mcs_entry));
   return n;
}

static inline void mcs_addtail (mcs_entry * e, mcs_entry * newe)
{
   assert (e);
   while (e->next)
   {
      e = e->next;
   }
   e->next = newe;
}

/* Add child to entry */
static void mcs_addchild (mcs_entry * section, mcs_entry * child)
{
   mcs_entry * p;

   p = section->child;
   if (!p)
   {
      section->child = child;
   }
   else
   {
      mcs_addtail (p, child);
   }
}


/* Create empty subsection */
static mcs_entry * mcs_createsection (const char * name)
{
   mcs_entry * n = mcs_allocentry ();
   n->is_section = 1;
   n->name = (name ? strdup (name) : 0);
   return n;
}

/* Create text entry */
static mcs_entry * mcs_createvalue (const char * name, const char ** values,
      unsigned int count)
{
   mcs_entry * n = mcs_allocentry ();
   unsigned int i;

   n->is_section = 0;
   n->next = 0;
   n->name = (name ? strdup (name) : 0);

   for (i=0; i<count; ++i)
   {
      mcs_entry * v = mcs_allocentry ()  ;
      v->is_section = 0;
      v->next = 0;
      v->name = 0;
      v->value = (values[i] ? strdup (values[i]) : 0);
      mcs_addchild (n, v);
   }

   return n;
}

/* Recursively free entry and all following entries and children */
static void mcs_freechain (mcs_entry * t)
{
   if (!t)
      return;

   while (t)
   {
      mcs_entry * old = t;

      free ((void*) t->name);
      if (t->is_section)
      {
         /* assert (!t->value); */ /* t->value and t->child are one...
                                      (union)*/
         mcs_freechain (t->child);
      }
      else
      {
         /* free all child values */
         mcs_entry * v = t->child;
         while (v)
         {
            mcs_entry * valentry = v;
            assert (!v->name);
            free ((void *) v->value);
            v = v->next;
            free (valentry);
         }
      }
      t = t->next;
      free (old);
   }
}

/* Remove (and free) named entry from section */
int mcs_removechild (mcs_entry * section, const char * child)
{
   mcs_entry * e;
   mcs_entry * prev;

   prev = 0;
   e = section->child;

   assert (e->is_section);
   if (!e->child)
      return 0;


   while (e)
   {
      if (!strcmp (child, e->name))
      {
         /* found entry */
         if (!prev)
         {
            /* entry is first child */
            section->child = e->next;
         }
         else
         {
            prev->next = e->next;
         }

         /* isolate e so we don't free the whole chain */
         e->next = 0;
         mcs_freechain (e);
         return 1;
      }
      e = e->next;
   }
   return 0;
}

/* Free (recursively) section */
int mcs_freeroot (mcs_entry * section)
{
   mcs_freechain (section);
   return 1;
}

static unsigned int mcs_chaincount (const mcs_entry * e)
{
   unsigned int ret = 0;

   while (e)
   {
      ++ret;
      e = e->next;
   }

   return ret;
}

/* ============= high level functions ======================== */

/* Return empty tree */
mcs_entry * mcs_initroot ()
{
   return mcs_createsection (0);
}
/* Add child section to section; returns child section */
mcs_entry * mcs_addsection (mcs_entry * e, const char * name)
{
   mcs_entry * newsec = mcs_createsection (name);
   mcs_addchild (e, newsec);
   return newsec;
}

/* Add (multi-)key to section */
mcs_entry * mcs_addkey (mcs_entry * e, const char * name, const char **
      values, unsigned int count)
{
   mcs_entry * newkey = mcs_createvalue (name, values, count);
   mcs_addchild (e, newkey);
   return newkey;
}


/* Returns true if entry is a subsection */
int mcs_issection (const mcs_entry * e)
{
   return e->is_section;
}

/* Returns number of children in section */
int mcs_childcount (const mcs_entry * e)
{
   assert (e->is_section);
   return mcs_chaincount (e->child);
}

/* Return the number of values in the key */
int mcs_valuecount (const mcs_entry * e)
{
   assert (!e->is_section);
   return mcs_chaincount (e->child);
}

/*
 * If buf & bufsize == 0, returns the length of the key value (without
 * terminating 0) or < 0 if error.
 * Returns len of key value without ending '\0'
 */
int mcs_getvaluesingle (const mcs_entry * e, char * buf, unsigned int bufsize)
{
   if (e->is_section)
      return MCS_WRONGTYPE;

   if (buf)
      *buf = 0;

   if (!e->child)
      return -1;

   if (!e->child->value)
      return 0;

   if (bufsize)
   {
      strncpy (buf, e->child->value, bufsize-1);
      buf[bufsize-1] = 0;
   }


   return strlen (e->child->value);
}

/* Retrieve the values for this key */
int mcs_getvaluemultiple (const mcs_entry * e, char ** buf, unsigned int * maxcount)
{
   unsigned int i = 0;
   mcs_entry * t = e->child;

   assert (!e->is_section);

   while (t)
   {
      if (i == *maxcount)
         return 0;

      assert (!t->name);

      buf[i] = (t->value ? strdup (t->value) : 0);
      ++i;
      t = t->next;
   }
   *maxcount = i;
   return 1;
}

/* Move to the next entry on this level */
mcs_entry * mcs_next (const mcs_entry * e)
{
   return e->next;
}

/* Move to the first child of this section */
mcs_entry * mcs_child (const mcs_entry * e)
{
   assert (e->is_section);
   return e->child;
}

/* Look for a child node with the specified name */
static mcs_entry * mcs_findchild (const mcs_entry * e, const char * name)
{
   mcs_entry * curchild = e->child;

   while (curchild)
   {
      if (!strcmp (curchild->name, name))
         break;
      curchild = curchild->next;
   }
   return curchild;
}


mcs_entry * mcs_findsubsection (const mcs_entry * e, const char * name)
{
   mcs_entry * ret = mcs_findchild (e, name);
   if (!ret)
      return 0;
   if (!ret->is_section)
      return 0;
   return ret;
}

mcs_entry * mcs_findkey (const mcs_entry * e, const char * name)
{
   mcs_entry * ret = mcs_findchild (e, name);
   if (!ret)
      return 0;
   if (ret->is_section)
      return 0;
   return ret;
}

int mcs_listsection (const mcs_entry * e, mcs_section_entry * out, unsigned int maxcount)
{
   const mcs_entry * cur;
   unsigned int i = 0;

   if (!e)
      return 0;
   if (!e->is_section)
      return -1;

   cur = e->child;

   i=0;
   while (cur && i < maxcount)
   {
      out[i].name = (cur->name ? strdup (cur->name) : 0);
      out[i].is_section = cur->is_section;
      out[i].childcount = mcs_chaincount (cur->child);
      ++i;
      cur = cur->next;
   }
   return i;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
