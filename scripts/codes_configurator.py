#!/usr/bin/python2.7

import re
import sys
import os
import imp
import argparse
from collections import Sequence

def main():
    args = parse_args()
    
    # load the template file
    tstr = open(args.template).read()

    # load the module
    mod = import_from(args.substitute_py)

    # checks - make sure cfields is set and is the correct type 
    if "cfields" not in mod.__dict__:
        raise TypeError("Expected cfields to be defined in " + args.substitute_py)
    elif not \
            (isinstance(mod.cfields, Sequence) and \
            isinstance(mod.cfields[0][0], str) and \
            isinstance(mod.cfields[0][1], Sequence)):
        raise TypeError("cfields in incorrect format, see usage")
    num_fields = len(mod.cfields)
    
    # get list of iterators, replacement input, and output labels
    labels      = [ k[0] for k in mod.cfields]
    iterables   = [ k[1].__iter__() for k in mod.cfields]
    replace_map =     { k[0] : None for k in mod.cfields }

    # process the command line token replacements, adding to labels
    # and replace_map, but not to iterables
    if len(args.token_pairs) % 2 != 0:
        raise ValueError("token pairs must come in twos")
    elif len(args.token_pairs) > 0:
        for i in range(0, len(args.token_pairs), 2):
            k,vstr = args.token_pairs[i],args.token_pairs[i+1]
            # try making v numeric - fall back to string
            v = try_num(vstr)
            if v == None:
                v = vstr
            # add pair to replace map and labels (but not iterables!)
            if k in replace_map:
                raise ValueError("token " + k + " of token_pairs matches a token "
                        "given by the substitution py file")
            else:
                replace_map[k] = v
                labels.append(k)

    # print the header to the log
    if args.log != None:
        flog = open(args.log, "w")
    else:
        flog = open(os.devnull, "w")
    flog.write("# format:\n# <config index>")
    for l in labels:
        flog.write(" <" + l + ">")
    flog.write("\n")

    # generate initial configuration 
    for i in range(0,num_fields):
        v = iterables[i].next()
        replace_map[labels[i]] = v

    # main loop 
    ct = 0
    while True:
        # generate new configuration
        new_config = replace_many(tstr, replace_map)
        fname = template+"."+str(ct) if args.output_prefix==None else \
            args.output_prefix+"."+str(ct)
        with open(fname, "w") as fout:
            fout.write(new_config)

        # print the configuration to the log
        flog.write(str(ct))
        for l in labels:
            flog.write(' ' + str(replace_map[l]))
        else:
            flog.write('\n')

        # generate the next config
        for i in range(0,num_fields):
            try:
                # update current iterable and finish
                v = iterables[i].next()
                replace_map[labels[i]] = v
                ct += 1
                break
            except StopIteration:
                # reset the current iterable and set to first element
                iterables[i] = mod.cfields[i][1].__iter__()
                v = iterables[i].next()
                replace_map[labels[i]] = v
        else:
            # last iterable has finished, have generated full set
            break

# import a python file (assumes there is a .py suffix!!!)
def import_from(filename):
    path,name = os.path.split(filename)
    name,ext  = os.path.splitext(name) 

    fmod, fname, data = imp.find_module(name, [path])
    return imp.load_module(name,fmod,fname,data)

# given a string and a set of substitution pairs, 
#   perform a single pass replace 
# st is the string to perform substitution on
# kv_pairs is a dict or a sequence of sequences.
# kv_pairs examples:
# { a:b, c:d }
# [[a,b],[c,d]]
# [(a,b),(c,d)]
# ((a,b),(c,d))
def replace_many(st, kv_pairs):
    rep_dict = {}
    # dict-ify and string-ify the input
    if isinstance(kv_pairs, dict):
        for k in kv_pairs:
            rep_dict[k] = str(kv_pairs[k])
    elif isinstance(kv_pairs, Sequence) and isinstance(kv_pairs[0], Sequence):
        for k in kv_pairs:
            rep_dict[k[0]] = str(kv_pairs[k[1]])
    else:
        raise TypeError("Expected dict or sequence of sequences types")

    pat = re.compile("|".join([re.escape(k) for k in rep_dict]))

    return pat.sub(lambda match: rep_dict[match.group(0)], st)

def parse_args():
    parser = argparse.ArgumentParser(\
            description="generate set of configuration files based on template "
                        "file and replacement tokens")
    parser.add_argument("template",
            help="template file with replace tokens")
    parser.add_argument("substitute_py",
            help='python file defining "cfields" variable consisting of '
                 'elements of the form '
                 '( replacement_token, [replacements...])')
    parser.add_argument("token_pairs",
            nargs='*',
            help="a list of whitespace-separated token, replace pairs for "
                 "command-line-driven replacement (useful in shell scripts "
                 "for generating sets of configuration files with a distinct "
                 "parameter or for special casing fields)")
    parser.add_argument("-l", "--log",
            help="log file to write parameterizations to")
    parser.add_argument("-o", "--output_prefix",
            help="prefix to output generated configuration files to "
                 "(default: the configuration index appended to the template name)")
    return parser.parse_args()

def try_num(s):
    try:
        return int(s)
    except ValueError:
        try:
            return float(s)
        except ValueError:
            return None

if __name__ == "__main__":
    main()
