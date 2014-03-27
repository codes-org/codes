#!/usr/bin/python2.7

import os
import argparse
import configurator as conf

def main():
    args = parse_args()
    
    # load the template file
    tstr = open(args.template).read()

    # load the module
    mod = conf.import_from(args.substitute_py)

    # checks - make sure cfields is set and is the correct type 
    conf.check_cfields(mod)

    # check that pairs are actually pairs
    if len(args.token_pairs) % 2 != 0:
        raise ValueError("token pairs must come in twos")

    # intitialize the configuration object 
    cfg = conf.configurator(mod, args.token_pairs)

    # print the header to the log
    if args.log != None:
        flog = open(args.log, "w")
    else:
        flog = open(os.devnull, "w")
    
    cfg.write_header(flog)

    # main loop (cfg iterator returns nothing, eval'd for side effects)
    for i,_ in enumerate(cfg):
        new_config = conf.replace_many(tstr, cfg.replace_map)
        fname = template+"."+str(i) if args.output_prefix==None else \
            args.output_prefix+"."+str(i)
        with open(fname, "w") as fout:
            fout.write(new_config)
        
        # write this config to the log 
        cfg.write_config_line(i, flog)

    flog.close()

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
    parser.add_argument("token_pairs", nargs='*',
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

if __name__ == "__main__":
    main()
