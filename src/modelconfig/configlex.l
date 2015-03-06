%{

#include <string.h>
#include "src/modelconfig/configglue.h"
#include "src/modelconfig/configparser.h"

#define YY_EXTRA_TYPE ParserParams * 

/* Disable warnings in the flex generated code */
#if defined __GNUC__
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#elif defined __SUNPRO_CC
#pragma disable_warn
#elif defined _MSC_VER
#pragma warning(push, 1)
#endif

#define CFGP_CHECKOVERFLOW 

#define LOC_COL_ADV(a) yylloc->last_column+=(a)
#define LOC_COL_DEF_ADV yylloc->last_column+=strlen(yytext)
#define LOC_COL_RESET  yylloc->last_column=1
#define LOC_LINE_ADV(a) yylloc->last_line+=(a),yylloc->last_column=1
#define LOC_RESET_FIRST yylloc->first_line=yylloc->first_column=0
#define LOC_INIT_FIRST yylloc->first_line=yylloc->last_line,\
                        yylloc->first_column=yylloc->last_column

#define LOC_INIT_LAST yylloc->last_line=1,yylloc->last_column=1

#define YY_USER_INIT LOC_INIT_LAST,LOC_INIT_FIRST; \
           yyextra->lexer_error_code=0; yyextra->lexer_error_string=0;

%}


%option reentrant stack noyywrap bison-locations 
%x COMMENT STRING_LITERAL COMMENT2
%option prefix="cfgp_"
%option bison-bridge nodefault

DIGIT   [0-9]
ID      [a-zA-Z][a-zA-Z0-9_\-]*(@[a-zA-Z0-9_\-]+)?

SECTION_OPEN  "{"
SECTION_CLOSE "}"
SEMICOLUMN    ";"
BACKSLASH     "\\"

%%

"/*"                 { 
                        LOC_COL_DEF_ADV;
                        LOC_INIT_FIRST;
                        /* ignore comments */
                        yy_push_state(COMMENT2, yyscanner);
                     }

<COMMENT2>"*/"       {
                        /* ignore comments */
                        LOC_RESET_FIRST;
                        yy_pop_state (yyscanner);  
                     }

<COMMENT2>\n          { LOC_LINE_ADV(1); } 

<COMMENT2>.+          { 
                        LOC_COL_DEF_ADV;
                        /* ignore comments */
                      }

<COMMENT2><<EOF>> {
                        char buf[512];
                        snprintf (buf,sizeof(buf),
                            "Unterminated /* (started at line %i, column %i)",
                            yylloc->first_line, yylloc->first_column);

                        return cfgp_lex_error (yyextra,
                              yylloc->last_line,yylloc->last_column,  buf);
                  }

"//"                 { LOC_COL_ADV(1); yy_push_state(COMMENT, yyscanner); }
"#"                  { LOC_COL_ADV(1); yy_push_state(COMMENT, yyscanner); }

<COMMENT>\n          { LOC_LINE_ADV(1); yy_pop_state( yyscanner ); }
<COMMENT>[^\n]+      { LOC_COL_DEF_ADV; }

\"                   { 
                        LOC_COL_DEF_ADV;
                        LOC_INIT_FIRST;
                        yylval->curstringpos = 0;
                        yy_push_state(STRING_LITERAL, yyscanner);
                     }


<STRING_LITERAL>\"        { 
                                 /* saw closing quote - all done */
                            LOC_COL_DEF_ADV;
                            LOC_RESET_FIRST;
                                 yy_pop_state(yyscanner);
                                 CFGP_CHECKOVERFLOW;
                                 yylval->string_buf[yylval->curstringpos] = '\0';
                                 /* return string constant token type and
                                  * value to parser
                                  */
                                 return LITERAL_STRING;
                           }

<STRING_LITERAL>\n        {
               char buf[512];
               snprintf (buf, sizeof(buf), 
                  "Unterminated string constant (started at line %i, column"
                  " %i)!", yylloc->first_line, yylloc->first_column);
               return cfgp_lex_error (yyextra, yylloc->last_line, yylloc->last_column,
                     buf);
        }

<STRING_LITERAL>\\[0-7]{1,3} {
        /* octal escape sequence */
        LOC_COL_DEF_ADV;
        int result;

        (void) sscanf( yytext + 1, "%o", &result );

        if ( result > 0xff )
        {
                /* error, constant is out-of-bounds */
                return cfgp_lex_error (yyextra, yylloc->last_line, yylloc->last_column,
                   "Out of bounds \\0xx escape in string!");
        }

        yylval->string_buf[yylval->curstringpos++] = result;
        CFGP_CHECKOVERFLOW;
        }

<STRING_LITERAL>\\[0-9]+ {
        /* generate error - bad escape sequence; something
         * like '\48' or '\0777777'
         */
                return cfgp_lex_error (yyextra, 
                    yylloc->last_line, yylloc->last_column,
                   "Bad \\xxxx escape in string!");
        }

<STRING_LITERAL>\\n  { LOC_COL_DEF_ADV; yylval->string_buf[yylval->curstringpos++] = '\n'; CFGP_CHECKOVERFLOW; }
<STRING_LITERAL>\\t  { LOC_COL_DEF_ADV; yylval->string_buf[yylval->curstringpos++] = '\t'; CFGP_CHECKOVERFLOW; }
<STRING_LITERAL>\\r  { LOC_COL_DEF_ADV; yylval->string_buf[yylval->curstringpos++] = '\r'; CFGP_CHECKOVERFLOW; }
<STRING_LITERAL>\\b  { LOC_COL_DEF_ADV; yylval->string_buf[yylval->curstringpos++] = '\b'; CFGP_CHECKOVERFLOW; }
<STRING_LITERAL>\\f  { LOC_COL_DEF_ADV; yylval->string_buf[yylval->curstringpos++] = '\f'; CFGP_CHECKOVERFLOW; }

<STRING_LITERAL>\\\n {
                LOC_LINE_ADV(1);
                yylval->string_buf[yylval->curstringpos++] = yytext[1];
                CFGP_CHECKOVERFLOW;
                }

<STRING_LITERAL>\\. {
                LOC_COL_DEF_ADV;
                yylval->string_buf[yylval->curstringpos++] = yytext[1];
                CFGP_CHECKOVERFLOW;
                }

<STRING_LITERAL>[^\\\n\"]+        {
        char *yptr = yytext;

        LOC_COL_DEF_ADV;

        while ( *yptr )
        {
                yylval->string_buf[yylval->curstringpos++] = *yptr++;
                CFGP_CHECKOVERFLOW;
        }
        }

<STRING_LITERAL>\\ { 
                   /* a single slash at the end of the input */
                   char buf[512];
                   snprintf (buf, sizeof(buf), "Unterminated string constant "
                      "(started at line %i, column %i)!", yylloc->first_line, 
                      yylloc->first_column);
                          
                   return cfgp_lex_error (yyextra, 
                          yylloc->last_line, yylloc->last_column, buf);
                 } 

{ID}    {
                LOC_COL_DEF_ADV;
                if (strlen (yytext) >= sizeof(yylval->string_buf))
                {
                     char buf[512];
                     snprintf (buf, sizeof(buf), "ID too long: Maximum ID"
                        " length is %lu bytes!", sizeof(yylval->string_buf));
                     return cfgp_lex_error (yyextra, 
                       yylloc->last_line, yylloc->last_column, buf);
                }
                strncpy (yylval->string_buf, yytext,  sizeof (yylval->string_buf)); 
                return IDENTIFIER;
                
        }

{SECTION_OPEN}      { LOC_COL_DEF_ADV; return OPENSECTION; }
{SECTION_CLOSE}     { LOC_COL_DEF_ADV; return CLOSESECTION; }
{SEMICOLUMN}        { LOC_COL_DEF_ADV; return SEMICOLUMN; }
"="                { LOC_COL_DEF_ADV; return EQUAL_TOKEN; }
","                { LOC_COL_DEF_ADV; return KOMMA; }
")"                { LOC_COL_DEF_ADV; return LCLOSE; }
"("                { LOC_COL_DEF_ADV; return LOPEN; }

[ \t]+              { 
                        /* ignore whitespace */
                        LOC_COL_DEF_ADV; 
                    }

\n                  {
                        /* ignore EOL */
                        LOC_LINE_ADV(1);
                    }


  /* we could try to return chars not matching any token to the parser and
   * have parser generate a useful error; tried that, parser gives output:
   *    syntax error, unexpected $undefined, expecting OPENSECTION or EQUAL_TOKEN
   * while the lexer error gives:
   *     lexer error (line 1, column 11): Unexpected character: ':'
   */


.                   { 
                        char buf[256];
                        snprintf (buf, sizeof(buf), "Unexpected character: '%c'", *yytext);
                        return cfgp_lex_error (yyextra, yylloc->last_line,
                          yylloc->last_column, buf);
                    }

  /* .                   { return (int) *yytext; }*/

%%

void cfgp_setfile (FILE * f, yyscan_t scanner)
{
   yyset_in (f, scanner);
}
