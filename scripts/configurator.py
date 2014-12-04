""" Helpers and utilities for use by codes_configurator and friends """

#
# Copyright (C) 2013 University of Chicago.
# See COPYRIGHT notice in top-level directory.
#
#

import re
import os
import imp
from collections import Sequence

class configurator:
    # note: object assumes input has been validated by caller, though it will
    #       still throw if it finds duplicate keys in the dict
    def __init__(self, module, replace_pairs):
        # checks - check cfields and friends
        check_cfields(module)
        # check that pairs are actually pairs
        if len(replace_pairs) % 2 != 0:
            raise ValueError("token pairs must come in twos")

        self.mod = module
        # simplify None-checking for cfields by generating an empty tuple
        # instead
        if self.mod.cfields == None:
            self.mod.cfields = ()
        self.num_fields = len(self.mod.cfields)
        self.labels = [k[0] for k in self.mod.cfields] + replace_pairs[0::2]
        self.replace_map = { k[0] : None for k in self.mod.cfields }
        self.start_iter = False
        self.in_iter    = False
        self.has_except = "excepts" in self.mod.__dict__
        self.has_derived = "cfields_derived_labels" in self.mod.__dict__ and \
                           "cfields_derived" in self.mod.__dict__

        for i in range(0, len(replace_pairs), 2):
            k,vstr = replace_pairs[i], replace_pairs[i+1]
            # add pair to replace map and labels (but not iterables!)
            if k in self.replace_map:
                raise ValueError("token " + k + " of token_pairs matches a token "
                        "given by the substitution py file")
            # try making v numeric - fall back to string
            v = try_num(vstr)
            if v == None:
                v = vstr
            self.replace_map[k] = v
            
        # initialize derived labels if necessary
        if self.has_derived:
            self.labels += [l for l in self.mod.cfields_derived_labels]


    def __iter__(self):
        self.start_iter = True 
        self.in_iter = True
        return self
    def next(self):
        if not self.in_iter:
            # either uninitialized or at the end of the road
            raise StopIteration 
        elif self.start_iter:
            # first iteration - initialize the iterators
            self.iterables = [k[1].__iter__() for k in self.mod.cfields]
            for i in range(0, self.num_fields):
                v = self.iterables[i].next()
                self.replace_map[self.labels[i]] = v
            self.start_iter = False
        else:
            # > first iteration, perform the updates
            # generate the next config
            for i in range(0,self.num_fields):
                try:
                    # update current iterable and finish
                    v = self.iterables[i].next()
                    self.replace_map[self.labels[i]] = v
                    break
                except StopIteration:
                    # reset the current iterable and set to first element
                    self.iterables[i] = self.mod.cfields[i][1].__iter__()
                    v = self.iterables[i].next()
                    self.replace_map[self.labels[i]] = v
            else:
                # last iterable has finished, have generated full set
                raise StopIteration 
        # add derived fields before exceptions 
        if self.has_derived:
            self.mod.cfields_derived(self.replace_map)
        # check if this is a valid config, if not, then recurse
        if self.has_except and is_replace_except(self.mod.excepts,
                self.replace_map):
            return self.next()
        else:
            return None

    def write_header(self, fout):
        fout.write("# format:\n# <config index>")
        for l in self.labels:
            fout.write(" <" + l + ">")
        fout.write("\n")

    def write_config_line(self, ident, fout):
        # print the configuration to the log
        fout.write(str(ident))
        for l in self.labels:
            fout.write(' ' + str(self.replace_map[l]))
        else:
            fout.write('\n')

def is_replace_except(except_map, replace_map):
    for d in except_map:
        for k in d:
            if d[k] != replace_map[k]:
                break
        else:
            return True
    return False

# checks - make sure cfields is set and is the correct type 
# note: cfields can be None or an empty sequence
def check_cfields(module):
    if "cfields" not in module.__dict__:
        raise TypeError("Expected cfields to be defined in " + str(module))
    elif module.cfields == None or \
            (isinstance(module.cfields, Sequence) and len(module.cfields) == 0):
        return
    elif not \
            (isinstance(module.cfields, Sequence) and \
            isinstance(module.cfields[0][0], str) and \
            isinstance(module.cfields[0][1], Sequence)):
        raise TypeError("cfields in incorrect format, see usage")

    if "excepts" in module.__dict__ and not \
            (isinstance(module.excepts, Sequence) and\
            isinstance(module.excepts[0], dict)) :
        raise TypeError("excepts not in correct format, see usage")

    dl = "cfields_derived_labels" in module.__dict__
    d  = "cfields_derived" in module.__dict__
    if (dl and not d) or (not dl and d):
        raise TypeError("both cfields_derived_labels and cfields_derived must "
                "be set")
    elif dl and d and not \
            (isinstance(module.cfields_derived_labels, Sequence) and \
            isinstance(module.cfields_derived_labels[0], str) and \
            hasattr(module.cfields_derived, "__call__")):
        raise TypeError("cfields_derived_labels must be a sequence of "
                "strings, cfields_derived must be callable (accepting a "
                "dict of replace_token, replacement pairs and adding pairs "
                "for each label in cfields_derived_labels")
        


# import a python file (assumes there is a .py suffix!!!)
def import_from(filename):
    path,name = os.path.split(filename)
    name,ext  = os.path.splitext(name) 

    fmod, fname, data = imp.find_module(name, [path])
    return imp.load_module(name,fmod,fname,data)

# attempts casting to a numeric type, returning None on failure
def try_num(s):
    try:
        return int(s)
    except ValueError:
        try:
            return float(s)
        except ValueError:
            return None

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
