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

    # get list of iterators, replacement input, and output labels
    labels      = [ k[0] for k in mod.cfields]
    iterables   = [ k[1].__iter__() for k in mod.cfields]
    # use two dicts to maintain type information when printing (don't want
    #  quotes to appear in log file)
    replace_map =     { k[0] : None for k in mod.cfields }
    replace_map_log = dict(replace_map)

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
    for i,k in enumerate(labels):
        v = iterables[i].next()
        replace_map_log[k] = v
        replace_map[k] = re.escape(str(v))

    # main loop 
    done = False
    ct = 0
    while not done:
        # generate new configuration
        new_config = replace_many(tstr, replace_map)
        fname = template+"."+str(ct) if args.output_prefix==None else \
            args.output_prefix+"."+str(ct)
        with open(fname, "w") as fout:
            fout.write(new_config)

        # print the configuration to the log
        flog.write(str(ct))
        for i,k in enumerate(labels):
            if isinstance(replace_map_log[k], str):
                flog.write(' "' + replace_map_log[k] + '"')
            else:
                flog.write(" " + str(replace_map_log[k]))
        else:
            flog.write('\n')

        # generate the next config
        for i,k in enumerate(labels):
            try:
                v = iterables[i].next()
                replace_map[k] = re.escape(str(v))
                replace_map_log[k] = v
                ct += 1
                break
            except StopIteration:
                iterables[i] = mod.cfields[i][1].__iter__()
                v = iterables[i].next()
                replace_map[k] = re.escape(str(v))
                replace_map_log[k] = v
        else:
            done = True

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
# { 1:2, 2:3 }
# [[1,2],[2,3]]
# [(1,2),(2,3)]
# ((1,2),(2,3))
def replace_many(st, kv_pairs):
    re.DEBUG = True
    rep_dict = dict(kv_pairs)
    repl = lambda match: rep_dict[match.group(0)]
    
    if isinstance(kv_pairs, dict):
        match_list = [re.escape(k) for k in kv_pairs]
    elif isinstance(kv_pairs, Sequence) and isinstance(kv_pairs[0], Sequence):
        match_list = [re.escape(k[0]) for k in kv_pairs]
    else:
        raise TypeError("Expected dict or sequence of sequences types")
    # get the list of keys 
    pat = re.compile("|".join(match_list))
    return pat.sub(repl, st)

def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("template",\
            help="template file with replace tokens")
    parser.add_argument("substitute_py",\
            help='python file defining "cfields" variable consisting of'\
                 ' elements of the form ( replacement_token, [replacements...])')
    parser.add_argument("-l", "--log",\
            help="log file to write parameterizations to") 
    parser.add_argument("-o", "--output_prefix",\
            help="prefix to output generated configuration files to "
                 "(default: the configuration index appended to the template name)")
    return parser.parse_args()


if __name__ == "__main__":
    main()
