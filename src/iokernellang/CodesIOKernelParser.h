/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef CODESIOKERNELPARSER_H
#define CODESIOKERNELPARSER_H

#include "codesparser.h"
#include "CodesIOKernelContext.h"

YYLTYPE *CodesIOKernel_get_lloc  (yyscan_t yyscanner);
int CodesIOKernel_lex_init (yyscan_t* scanner);
int CodesIOKernel_lex(YYSTYPE * lvalp, YYLTYPE * llocp, void * scanner);
//YY_BUFFER_STATE CodesIOKernel__scan_string (yyconst char *yy_str ,yyscan_t yyscanner);

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
