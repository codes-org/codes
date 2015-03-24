/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

%{
#include <stdlib.h>
#include "src/iokernellang/CodesIOKernelTypes.h"
#include "src/iokernellang/CodesIOKernelParser.h"

#define YY_NO_INPUT

%}

%option reentrant
%option prefix="CodesIOKernel_"
%option bison-bridge
%option bison-locations
%option noyywrap
%option yylineno
%option nounput

%{
	YYLTYPE *CodesIOKernel_get_lloc  (yyscan_t yyscanner);
	int CodesIOKernel_lex_init (yyscan_t* scanner);
	int CodesIOKernel_lex(YYSTYPE * lvalp, YYLTYPE * llocp, void * scanner);
	YY_BUFFER_STATE CodesIOKernel__scan_string (yyconst char *yy_str ,yyscan_t yyscanner);

	#define YY_EXTRA_TYPE CodesIOKernelContext*
#if 0
	#define YY_USER_ACTION \
        do { \
                yylloc->first_line = yylloc->last_line = 1; \
                yylloc->first_column = yylloc->last_column = 0; \
        } while(0) ;
#else
        #define YY_USER_ACTION /* no user action */;
#endif

%}

%%

[a-z]       {
		if(yyextra->text != NULL)
                {
			free(yyextra->text);
                }
		yyextra->text = strdup(yytext);
                yylval->sIndex = *(yyextra->text) - 'a';
                return VARIABLE;
            }

0           {
		if(yyextra->text != NULL)
                {
			free(yyextra->text);
                }
		yyextra->text = strdup(yytext);
                yylval->iValue = atoi(yyextra->text);
                return INTEGER;
            }

[1-9][0-9]* {
		if(yyextra->text != NULL)
                {
			free(yyextra->text);
                }
		yyextra->text = strdup(yytext);
                yylval->iValue = atoll(yyextra->text);
                return INTEGER;
            }

[-()<>=+*/%,;{}.] {
		if(yyextra->text != NULL)
                {
			free(yyextra->text);
                }
		yyextra->text = strdup(yytext);
                return *(yyextra->text);
             }

">="            return GE;
"<="            return LE;
"=="            return EQ;
"!="            return NE;
"while"         return WHILE;
"if"            return IF;
"else"          return ELSE;
"print"         return PRINT;
"write"         return WRITE;
"writeat"       return WRITEAT;
"write_all"     return WRITE_ALL;
"writeat_all"   return WRITEAT_ALL;
"read"          return READ;
"readat"        return READAT;
"read_all"      return READ_ALL;
"readat_all"    return READAT_ALL;
"sync"          return SYNC;
"delete"        return DELETE;
"sleep"         return SLEEP;
"open"          return OPEN;
"close"         return CLOSE;
"flush"         return FLUSH;
"seek"          return SEEK;
"getgroupid"    return GETGROUPID;
"getnumgroups"  return GETNUMGROUPS;
"getgrouprank"  return GETGROUPRANK;
"getgroupsize"  return GETGROUPSIZE;
"getcurtime"    return GETCURTIME;
"exit"    	return EXIT;

[ \t\n]+        ;       /* ignore whitespace */

.               fprintf(stderr, "Unknown character\n");
%%

void CodesIOKernelScannerInit(CodesIOKernelContext * context)
{
	yylex_init(&context->scanner_);
	yyset_extra(context, context->scanner_);
	context->text = NULL;
	context->lval = malloc(sizeof(YYSTYPE));
	context->locval = CodesIOKernel_get_lloc(*(yyscan_t *)(context->scanner_));

        //((YYLTYPE *)context->locval)->first_line = 1;

	int i = 0;
	for(i = 0 ; i < 26 ; i++)
	{
	    context->sym[i] = 0;
	}

        for(i = 0 ; i < 10 ; i++)
        {
            context->next_var[i] = 0;
        }

        for(i = 0 ; i < 10 ; i++)
        {
            context->var[i] = 0;
        }

        context->inst_ready = 0;
}

void CodesIOKernelScannerDestroy(CodesIOKernelContext * context)
{
	yylex_destroy(context->scanner_);
	free(context->text);
	context->text = NULL;
	context->locval = NULL;
	context->lval = NULL;
}

void CodesIOKernelScannerSetSymTable(CodesIOKernelContext * context)
{
	sym = &(context->sym[0]);
        var = &(context->var[0]);
        inst_ready = &(context->inst_ready);
        group_rank = &(context->GroupRank);
        group_size = &(context->GroupSize);
}
