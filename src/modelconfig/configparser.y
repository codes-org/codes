
%pure-parser
%error-verbose
%locations

%parse-param {yyscan_t * scanner}
%lex-param {yyscan_t * scanner}
%parse-param {ParserParams * param}

// Note lower versions might also work
%require "2.3"

%name-prefix="cfgp_"
%defines

%{


#include <assert.h>
#include "src/modelconfig/configglue.h"

#if defined __GNUC__
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined __SUNPRO_CC
#pragma disable_warn
#elif defined _MSC_VER
#pragma warning(push, 1)
#endif


%}

%code requires {
  
  #ifndef YY_TYPEDEF_YY_SCANNER_T
  #define YY_TYPEDEF_YY_SCANNER_T
  typedef void* yyscan_t;
  #endif
   
}
    

%union {
        struct
        {
            char string_buf [512];
            unsigned int curstringpos;
        };

}

%{
#include "src/modelconfig/configlex.h"

int cfgp_error (YYLTYPE * loc, yyscan_t * scanner, ParserParams * p, 
                        const char * msg)
{
   if (loc)
   {
     return cfgp_parser_error (p, msg, loc->first_line, 
     loc->first_column, loc->last_line, loc->last_column);
   }
   else
   {
     return cfgp_parser_error (p, msg, 0,0,0,0);
   }
}

%}

%start configfile
%token <string_buf> LITERAL_STRING
%token OPENSECTION
%token CLOSESECTION
%token <string_buf> IDENTIFIER
%token EQUAL_TOKEN
%token SEMICOLUMN
%token KOMMA
%token LOPEN
%token LCLOSE


%initial-action {
   param->stacktop = 0;
   param->sectionstack[0] = 0;
   param->parser_error_code = 0;
   param->parser_error_string = 0;
}



%%

initdummy: /* empty */ {
                /* used to initialize vars */
                param->stacktop = 0;
          } 

configfile: initdummy sectioncontents ;

sectioncontents: sectioncontents entry | ;

entry: key | subsection;


key: singlekey | multikey  ;
  
singlekey: IDENTIFIER EQUAL_TOKEN LITERAL_STRING SEMICOLUMN {
                const char * key = & $<string_buf>1 [0];
                const char * value = & $<string_buf>3 [0];
                cf_createKey (param->configfile,
                      param->sectionstack[param->stacktop], key, &value, 1);
         }

multikeynonzero: KOMMA LITERAL_STRING {
                   param->keyvals[param->count++] = strdup ($<string_buf>2);
                   assert (param->count < param->maxsize); 
               }

multikeyentry : multikeynonzero multikeyentry | ;

multikeyinit : /* empty */ {
             param->maxsize = 1000;
             param->count = 0;
             param->keyvals = (char**) malloc (sizeof(char*)*param->maxsize);
             }

multikeystart : LITERAL_STRING  { 
                param->keyvals[param->count++] = strdup ($<string_buf>1);
                assert (param->count < param->maxsize); 
             }

/* this can probably be simplified */
multikeybody: multikeystart multikeyentry | ; 
          
multikey: IDENTIFIER EQUAL_TOKEN LOPEN multikeyinit multikeybody LCLOSE
        SEMICOLUMN {
                unsigned int i;
               /* when this is reduced we have all the keys */
               const char * key = & $<string_buf>1 [0];
                const char ** value = (const char **) param->keyvals;
                cf_createKey (param->configfile,
                      param->sectionstack[param->stacktop], key, value,
                      param->count);

                /* can free strings */
                for (i=0; i<param->count; ++i)
                {
                        free (param->keyvals[i]);
                }
                free (param->keyvals);
                param->keyvals = 0;
            }

opt_semicolumn: SEMICOLUMN | ;

subsection_openaction: IDENTIFIER OPENSECTION
                     {
                         SectionHandle newsection;
                         assert(param->stacktop < sizeof(param->sectionstack)/sizeof(param->sectionstack[0]));
                         
                         cf_createSection (param->configfile,
                         param->sectionstack[param->stacktop], $1,
                         &newsection);

                         /*cf_openSection (param->configfile,
                             param->sectionstack[param->stacktop], $1, &newsection); */
                         param->sectionstack[++param->stacktop] = newsection;
                     }; 

subsection_closeaction: CLOSESECTION opt_semicolumn
                      {
                          assert (param->stacktop > 0);
                          SectionHandle old = param->sectionstack[param->stacktop--];
                          cf_closeSection (param->configfile, old);
                      };

subsection: subsection_openaction sectioncontents subsection_closeaction ;


%%


