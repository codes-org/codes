AC_DEFUN([AX_PROG_BISON_CLFEATURES], [
	AC_REQUIRE([AC_PROG_YACC])
	AC_REQUIRE([AC_PROG_SED])

	AC_CACHE_CHECK([if bison is the parser generator],[ax_cv_prog_bison],[
  		AS_IF([test "`echo \"$YACC\" | $SED 's,^.*\(bison\).*$,\1,'`" = "bison" ],[
			ax_cv_prog_bison=yes
    			],[
      				ax_cv_prog_bison=no
    		])
  	])

cat > conftest.y <<ACEOF
%{
    int yylex(void*);
    void yyerror(const char *s);
%}
%pure-parser
%token  FIRST_TOK
%token  LAST_TOK
%start top
%%
top: FIRST_TOK LAST_TOK
%%
ACEOF

# set up some common variables for testing:
ac_cv_prog_yacc_root="y.tab"
ac_compile_yacc='$CC -c $CFLAGS $CPPFLAGS $ac_cv_prog_yacc_root.c >&5'

HAVE_YACC_OLD_PURE=
HAVE_YACC_OLD_PUSH=
AC_MSG_CHECKING([if ${YACC} supports pure / reentrant paser features])
if $YACC -d -t -v conftest.y > /dev/null 2>&1 && eval "$ac_compile_yacc"
then
	AC_SUBST([CODES_PURE_PARSER_DEFINES], ["%pure-parser"])
	AC_MSG_RESULT([old-style])
	$3
else

cat > conftest.y <<ACEOF
%{
    int yylex(void*);
    void yyerror(const char *s);
%}
%define api.pure
%token  FIRST_TOK
%token  LAST_TOK
%start top
%%
top: FIRST_TOK LAST_TOK
%%
ACEOF

# set up some common variables for testing:
ac_cv_prog_yacc_root="y.tab"
ac_compile_yacc='$CC -c $CFLAGS $CPPFLAGS $ac_cv_prog_yacc_root.c >&5'
	if $YACC -d -t -v conftest.y > /dev/null 2>&1 && eval "$ac_compile_yacc"
	then
		AC_SUBST([CODES_PURE_PARSER_DEFINES], ["%define api.pure"])
		AC_MSG_RESULT([new-style])
		$3
	else
		AC_MSG_RESULT([feature not supported])
		BVER=`${YACC} --version | head -n 1`
		AC_MSG_WARN([${BVER} does not support pure / reentrant parser generation])
		$4
	fi
fi

cat > conftest.y <<ACEOF
%{
    int yylex(void*);
    void yyerror(const char *s);
%}
%define api.push_pull "push"
%token  FIRST_TOK
%token  LAST_TOK
%start top
%%
top: FIRST_TOK LAST_TOK
%%
ACEOF

# set up some common variables for testing:
ac_cv_prog_yacc_root="y.tab"
ac_compile_yacc='$CC -c $CFLAGS $CPPFLAGS $ac_cv_prog_yacc_root.c >&5'

AC_MSG_CHECKING([if ${YACC} supports push parser features])
if $YACC -d -t -v conftest.y > /dev/null 2>&1 && eval "$ac_compile_yacc"
then
	AC_SUBST([CODES_PUSH_PARSER_DEFINES], ["%define api.push_pull \"push\""])
	AC_MSG_RESULT([old-style])
	$3
else
 
cat > conftest.y <<ACEOF
%{
    int yylex(void*);
    void yyerror(const char *s);
%}
%define api.push-pull push
%token  FIRST_TOK
%token  LAST_TOK
%start top
%%
top: FIRST_TOK LAST_TOK
%%
ACEOF

# set up some common variables for testing:
ac_cv_prog_yacc_root="y.tab"
ac_compile_yacc='$CC -c $CFLAGS $CPPFLAGS $ac_cv_prog_yacc_root.c >&5'
	if $YACC -d -t -v conftest.y > /dev/null 2>&1 && eval "$ac_compile_yacc"
	then
		AC_SUBST([CODES_PUSH_PARSER_DEFINES], ["%define api.push-pull push"])
		AC_MSG_RESULT([new-style])
		$3
	else
		AC_MSG_RESULT([feature not supported])
		BVER=`${YACC} --version | head -n 1`
		AC_MSG_WARN([${BVER} does not support push parser generation])
		$4
	fi
fi

  AS_IF([test "$ax_cv_prog_bison" = yes],[
    :
    $1
  ],[
    :
    $2
  ])

  # cleanup bison / yacc tmp files
  rm -rf y.output y.tab.h y.tab.c y.tab.o
])
