# Copyright (c) Neil McGlohon 2019
# Rensselaer Polytechnic Institute
# This script is an example pattern generator for use with the congestion controller module
# in CODES. It generates patterns, one per line, that when matched, indicate that the network
# is in a state of congestion. These patterns are used by the supervisory controller, looking
# at the last N number of measurement periods (epochs) for whether the ports or NICs experienced
# congestion, if the pattern of the last N measurement periods matches any pattern provided by
# this file, then congestion has been detected in the network.
# This is a brute force attempt but since these files only need to be generated once, it's not
# so important that this be the most optimized that it could be.

import sys

N_PERIODS = 5 #length of patterns
MIN_TO_INDICATE = 3 #how many measurement periods must be 'congested' for the network to consider itself in a state of congestion
HISTERISIS = 3 #how many time periods must be empty starting from the latest for the network to consider itself rid of congestion
QUIET = True

def log(s):
    if not QUIET:
        print(s)


def check_valid_congestion_pattern(pattern):
    # print("Checking: %s"%pattern)

    num_ones = pattern.count('1')
    if num_ones < MIN_TO_INDICATE:
        log("NOT CONGESTION: %s"%pattern)
        return False
    
    num_ones_in_histerisis_window = pattern[-HISTERISIS:].count('1')
    if num_ones_in_histerisis_window < 1:
        log("NOT CONGESTION: %s"%pattern)
        return False
    
    log("CONGESTION: %s"%pattern)
    return True

def check_valid_decongestion_pattern(pattern):

    num_ones_in_histerisis_window = pattern[-HISTERISIS:].count('1')
    if num_ones_in_histerisis_window < 1:
        log("DECONGESTION: %s"%pattern)
        return True

def generate_patterns():
    valid_congestion_patterns = []
    valid_decongestion_patterns = []

    format_specifier = '#0%db'%(N_PERIODS+2)
    max_number = 2**N_PERIODS
    for i in range(max_number):
        pattern = format(i, format_specifier)[2:]

        if check_valid_congestion_pattern(pattern):
            valid_congestion_patterns.append(pattern)

        if check_valid_decongestion_pattern(pattern):
            valid_decongestion_patterns.append(pattern)

    return (valid_congestion_patterns, valid_decongestion_patterns)

def write_patterns(filename, patterns):
    with open(filename,"w") as f:
        for pattern in patterns:
            # log("Writing: %s"%pattern)
            f.write(pattern+'\n')

def main():
    if len(sys.argv) < 5:
        print("Usage: python3 get_pattern_file.py <length of patterns> <min to indicate> <histerisis parameter> <output congestion filename> <output decongestion filename> (optional: --verbose)")
        exit(1)

    global QUIET
    if '--verbose' in sys.argv:
        QUIET = False

    global N_PERIODS, MIN_TO_INDICATE, HISTERISIS
    N_PERIODS = int(sys.argv[1])
    MIN_TO_INDICATE = int(sys.argv[2])
    HISTERISIS = int(sys.argv[3])
    congestion_filename = sys.argv[4]
    decongestion_filename = sys.argv[5]


    print("Generating: Length of Patterns=%d  Minimum to Indicate=%d  Histerisis=%d"%(N_PERIODS, MIN_TO_INDICATE, HISTERISIS))

    (congestion_patterns, decongestion_patterns) = generate_patterns()
    write_patterns(congestion_filename, congestion_patterns)
    write_patterns(decongestion_filename, decongestion_patterns)

    print("Written to %s and %s"%(congestion_filename,decongestion_filename))

if __name__ == "__main__":
    main()