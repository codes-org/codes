""" Helpers and utilities for use by codes_configurator and friends """

import re
import os
import imp
from collections import Sequence

class configurator:
    # note: object assumes input has been validated by caller, though it will
    #       still throw if it finds duplicate keys in the dict
    def __init__(self, module, replace_pairs):
        self.mod = module
        self.num_fields = len(self.mod.cfields)
        self.labels = [k[0] for k in self.mod.cfields] + replace_pairs[0::2]
        self.replace_map = { k[0] : None for k in self.mod.cfields }
        self.start_iter = False
        self.in_iter    = False

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

    def __iter__(self):
        # pre-generate an initial config and return self
        self.start_iter = True 
        self.in_iter = True
        return self
    def next(self):
        if not self.in_iter:
            # either uninitialized or at the end of the road
            raise StopIteration 
        elif self.start_iter:
            # first iteration - initialize the iterators
            self.iterables   = [k[1].__iter__() for k in self.mod.cfields]
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

# checks - make sure cfields is set and is the correct type 
def check_cfields(module):
    if "cfields" not in module.__dict__:
        raise TypeError("Expected cfields to be defined in " + str(module))
    elif not \
            (isinstance(module.cfields, Sequence) and \
            isinstance(module.cfields[0][0], str) and \
            isinstance(module.cfields[0][1], Sequence)):
        raise TypeError("cfields in incorrect format, see usage")

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
