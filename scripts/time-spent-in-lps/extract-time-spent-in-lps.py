#!/usr/bin/env python3

# This is a script to extract the CPU time spent on each LP type (workload, terminal and routers)

import argparse
import re
import fileinput
import glob
from typing import NamedTuple, Callable
from collections import defaultdict
import io


class ComputeStats(NamedTuple):
    time: float
    events: int

class LPStats(NamedTuple):
    aggregated: ComputeStats
    buckets: dict[int, ComputeStats]

class SimStats(NamedTuple):
    lps: dict[int, LPStats]
    buckets_used: set[int]

def get_lps_processing_time(
    filehandler: fileinput.FileInput[str]
) -> SimStats:
    lp_line_re = re.compile(r"LPID: ([0-9]+)\s+-  Processing time: ([^ \t]+)" +
                            r"\s+-  Events processed: ([0-9]+)")
    lp_bucket_re = re.compile(
            r"LPID: ([0-9]+)\s+-  Bucket: ([0-9]+)\s+  Processing time: ([^ \t]+)" +
            r"\s+-  Events processed: ([0-9]+)")

    lp_data: dict[int, LPStats] = defaultdict(lambda: LPStats(ComputeStats(-1, -1), {}))
    buckets_used: set[int] = set()

    for line in filehandler:
        lp_line = lp_line_re.match(line)
        lp_bucket = lp_bucket_re.match(line)
        if lp_line:
            assert lp_bucket is None
            lpid = int(lp_line[1])
            proc_time = float(lp_line[2])
            proc_events = int(lp_line[3])

            buckets = lp_data[lpid].buckets if lpid in lp_data else {}
            lp_data[lpid] = LPStats(ComputeStats(proc_time, proc_events), buckets)
        elif lp_bucket:
            lpid = int(lp_bucket[1])
            bucket = int(lp_bucket[2])
            proc_time = float(lp_bucket[3])
            proc_events = int(lp_bucket[4])

            lp_data[lpid].buckets[bucket] = ComputeStats(proc_time, proc_events)
            buckets_used.add(bucket)

    return SimStats(lp_data, buckets_used)


class AggregateStats(NamedTuple):
    total_nw_lp_time: float
    total_terminal_time: float
    total_router_time: float
    total_nw_lp_n: int
    total_terminal_n: int
    total_router_n: int

    def __repr__(self):  # type: ignore[reportImplicitOverride]
        output = io.StringIO()
    
        if self.total_nw_lp_time or self.total_terminal_time or self.total_router_time:
            print("Total time spent on (in sec):", file=output, end='')
        if self.total_nw_lp_time:
            print("\nnw-lps    =", self.total_nw_lp_time, file=output, end='')
        if self.total_terminal_time:
            print("\nterminals =", self.total_terminal_time, file=output, end='')
        if self.total_router_time:
            print("\nrouters   =", self.total_router_time, file=output, end='')

        if self.total_nw_lp_n or self.total_terminal_n or self.total_router_n:
            print("\nTotal events processed:", file=output, end='')
        if self.total_nw_lp_n:
            print("\nnw-lps    =", self.total_nw_lp_n, file=output, end='')
        if self.total_terminal_n:
            print("\nterminals =", self.total_terminal_n, file=output, end='')
        if self.total_router_n:
            print("\nrouters   =", self.total_router_n, file=output, end='')

        if self.total_nw_lp_time or self.total_terminal_time or self.total_router_time:
            print("\nProcessing rate:", file=output, end='')
        if self.total_nw_lp_time:
            print("\nnw-lps    =", self.total_nw_lp_n / self.total_nw_lp_time, file=output, end='')
        if self.total_terminal_time:
            print("\nterminals =", self.total_terminal_n / self.total_terminal_time, file=output, end='')
        if self.total_router_time:
            print("\nrouters   =", self.total_router_n / self.total_router_time, file=output, end='')
    
        return output.getvalue()


def aggregate_stats(
        sim_stats: SimStats,
        key: Callable[[LPStats], ComputeStats | None] | None = None
) -> AggregateStats:
    if key is None:
        key = lambda lpdata: lpdata.aggregated

    total_nw_lp_time = 0.0
    total_terminal_time = 0.0
    total_router_time = 0.0

    total_nw_lp_n = 0
    total_terminal_n = 0
    total_router_n = 0

    for lpid, lpdata in sim_stats.lps.items():
        pos_in_rep = lpid % elems_per_rep

        stats = key(lpdata)
        if stats is None:
            continue

        if pos_in_rep < nw_lp_n:
            total_nw_lp_time += stats.time
            total_nw_lp_n += stats.events
        elif pos_in_rep < nw_lp_n + terminal_n:
            total_terminal_time += stats.time
            total_terminal_n += stats.events
        else:
            total_router_time += stats.time
            total_router_n += stats.events

    return AggregateStats(
            total_nw_lp_time, total_terminal_time, total_router_time,
            total_nw_lp_n, total_terminal_n, total_router_n)




if __name__ == '__main__':
    # Claude helped to implement the argparse part
    parser = argparse.ArgumentParser()
    _ = parser.add_argument("dirname", help="path to ross_all_lp_stats")
    _ = parser.add_argument("--nw_lp_n", type=int, default=8, help="number of nw-lp")
    _ = parser.add_argument("--terminal_n", type=int, default=8, help="number of terminals")
    _ = parser.add_argument("--router_n", type=int, default=1, help="number of routers")
    _ = parser.add_argument("--repetitions", type=int, default=1056, help="number of repetitions")
    args = parser.parse_args()

    nw_lp_n = args.nw_lp_n
    terminal_n = args.terminal_n
    router_n = args.router_n
    elems_per_rep = nw_lp_n + terminal_n + router_n

    inputs = glob.glob(f'{args.dirname}/pe_*.txt')
    input_files = fileinput.input(inputs)
    sim_stats = get_lps_processing_time(input_files)

    assert elems_per_rep * args.repetitions == len(sim_stats.lps), \
        f"{elems_per_rep * args.repetitions} should be {len(sim_stats.lps)}"

    print("Network size of =", terminal_n * args.repetitions)
    print("Total LPs =", elems_per_rep * args.repetitions)
    print()
    print(aggregate_stats(sim_stats))
    for bucket in sim_stats.buckets_used:
        print()
        print(f"Bucket {bucket}")
        print(aggregate_stats(sim_stats, key=lambda lpdata: lpdata.buckets[bucket] if bucket in lpdata.buckets else None))
