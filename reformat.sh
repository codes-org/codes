#!/bin/bash

script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

function error () {
    echo "ERROR: $@" >&2
    exit 1
}

function usage() {
    cat \
<<EOF
USAGE: reformat.sh [-x] [-i] [-c CFG] [-b FILE] {-a | FILE...}
    -i: perform in-place (will ask to confirm if being done recursively)
    -c CFG: use CFG as the uncrustify config (default: $script_dir/uc-codes.cfg)
    -b FILE[,...]: add FILE to the blacklist of directories (in the form *FILE/*)
        multiple files can be added by separating with commas
    -a: perform recursively on all files ending in .c or .h
        if -a not provided, then perform on all files passed in
    -x: instead of formatting, check if output format is the same as input
    -h: show this help and exit
EOF
}

function build_blacklist() {
    for x in $@ ; do
        echo -n "-and -not -path \*$x/\* "
    done
}

function getfiles() {
    find . \( -name \*.c -or -name \*.h \) $(build_blacklist $@)
}

cfg=$script_dir/uc-codes.cfg

which uncrustify > /dev/null 2>&1 || error "uncrustify not found"

do_inplace=no
do_recursive=no
do_check=no
blacklist=

while getopts ":ib:axh" opt; do
    case $opt in
        i)
            do_inplace=yes
            ;;
        b)
            blacklist=$(cat $OPTARG | sed s/,/ /g)
            ;;
        c)
            cfg=$OPTARG
            ;;
        a)
            do_recursive=yes
            ;;
        x)
            do_check=yes
            ;;
        h)
            usage
            exit 1
            ;;
        \?)
            usage
            error "invalid argument: -$OPTARG"
            ;;
    esac
done

shift $((OPTIND-1))

[[ -e $cfg ]] || ( usage && error "config file $cfg not found" )

[[ $# -gt 0 && $do_recursive == yes ]] && \
    usage && error "no file args expected in recursive mode"
[[ $# -eq 0 && $do_recursive == no ]] && \
    usage && error "expected file args in non-recursive mode"

if [[ $do_recursive == yes ]] ; then
    file_list="$(getfiles $blacklist)"
else
    file_list="$@"
fi

if [[ $do_recursive == yes && $do_inplace == yes ]] ; then
    echo -n "Do recursive in-place reformat? (y/n) "
    read answer
    [[ $answer =~ "[^y].*" ]] && echo "...aborting reformat" && exit 1
fi

[[ $do_inplace == yes ]] \
    && output_arg="--replace" \
    || output_arg=

if [[ $do_check == yes ]] ; then
    check_arg=--check
    output_arg=
    echo "checking format..."
else
    check_arg=
fi

function echolines () {
    for x in $@ ; do echo $x ; done
}

echolines $file_list | uncrustify $check_arg -c $cfg $output_arg -F -
