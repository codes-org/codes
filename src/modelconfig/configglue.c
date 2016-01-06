/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "configglue.h"

int cfgp_lex_error (ParserParams * p, int lineno, int colno, const char * msg)
{
   char buf[512];
   snprintf (buf, sizeof(buf), "lexer error (line %i, column %i): %s", lineno,
           colno, msg);
   p->lexer_error_string = strdup (buf);
   p->lexer_error_code = 2;
   return -1;
}

int cfgp_parser_error(ParserParams * p, const char * err, 
      unsigned int first_line, unsigned int first_column,
      unsigned int last_line, unsigned int last_column)
{
   char location[128];
   char buf[512];

   if (first_line)
   {
      snprintf (location, sizeof(location), "%i:%i until %i:%i",
            first_line, first_column, last_line, last_column);
   }
   else if (last_line)
   {
      snprintf (location, sizeof(location), "line %i, column %i", 
            last_line, last_column);
   }
   else
   {
      strcpy (buf, "unknown location");
   }

   snprintf (buf, sizeof(buf), "Parser error (%s): %s", location, err);
   p->parser_error_code =1;
   p->parser_error_string = strdup (buf);
   return -1;
}

void cfgp_initparams (ParserParams * p, struct ConfigVTable * h)
{
   p->configfile = h;
   p->stacktop = 0;
   p->sectionstack[0] = 0;
   p->parser_error_code = 0;
   p->parser_error_string = 0;
   p->lexer_error_code=0;
   p->lexer_error_string=0;
}

void cfgp_freeparams (ParserParams * p)
{
   free (p->lexer_error_string);
   free (p->parser_error_string);
}

int cfgp_parse_ok (const ParserParams * p, char * buf, int bufsize)
{
   /* doublecheck that if an error string is present, the error code is also
    * set */
   assert(!p->lexer_error_string || p->lexer_error_code);
   assert(!p->parser_error_string || p->parser_error_code);

   if (p->lexer_error_code)
   {
      strncpy (buf, p->lexer_error_string, bufsize);
      return 0;
   }
   if (p->parser_error_code)
   {
      strncpy (buf, p->parser_error_string, bufsize);
      return 0;
   }
   return 1;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
