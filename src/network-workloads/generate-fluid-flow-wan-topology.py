#!/usr/bin/env python3
"""
Generate a WAN-like topology YAML file for model-net-fluid-flow-wan.

The generated graph is intentionally not a symmetric supercomputer-style
topology. It starts with a bidirectional ring to guarantee strong connectivity,
then adds random directed/asymmetric switch-to-switch links. Terminal access
links are generated with higher bandwidth than switch-to-switch links.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path


def positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"expected integer, got {value!r}") from exc
    if parsed <= 0:
        raise argparse.ArgumentTypeError(f"expected positive integer, got {parsed}")
    return parsed


def nonnegative_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"expected integer, got {value!r}") from exc
    if parsed < 0:
        raise argparse.ArgumentTypeError(f"expected nonnegative integer, got {parsed}")
    return parsed


def positive_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"expected float, got {value!r}") from exc
    if parsed <= 0.0:
        raise argparse.ArgumentTypeError(f"expected positive float, got {parsed}")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a random WAN-like topology YAML for model-net-fluid-flow-wan."
    )

    parser.add_argument(
        "--switches",
        type=positive_int,
        default=64,
        help="number of switches to generate; default: 64",
    )
    parser.add_argument(
        "--terminals-per-switch",
        type=positive_int,
        default=2,
        help="number of terminals attached to each switch; default: 2",
    )
    parser.add_argument(
        "--avg-switch-degree",
        type=positive_float,
        default=3.0,
        help=(
            "target average directed switch out-degree, including the ring links; "
            "default: 3.0"
        ),
    )
    parser.add_argument(
        "--reverse-link-probability",
        type=positive_float,
        default=0.35,
        help=(
            "probability of adding the reverse direction for each random extra link; "
            "default: 0.35"
        ),
    )
    parser.add_argument(
        "--switch-link-min-mbps",
        type=positive_float,
        default=10000.0,
        help="minimum switch-to-switch bandwidth in Mbps; default: 10000 (10 Gbps)",
    )
    parser.add_argument(
        "--switch-link-max-mbps",
        type=positive_float,
        default=30000.0,
        help="maximum switch-to-switch bandwidth in Mbps; default: 30000 (30 Gbps)",
    )
    parser.add_argument(
        "--terminal-link-min-mbps",
        type=positive_float,
        default=100000.0,
        help="minimum terminal-to-switch bandwidth in Mbps; default: 100000 (100 Gbps)",
    )
    parser.add_argument(
        "--terminal-link-max-mbps",
        type=positive_float,
        default=100000.0,
        help="maximum terminal-to-switch bandwidth in Mbps; default: 100000 (100 Gbps)",
    )
    parser.add_argument(
        "--switch-buffer-min-mb",
        type=positive_float,
        default=64000.0,
        help="minimum shared switch buffer in Mbit; default: 64000 (64 Gb)",
    )
    parser.add_argument(
        "--switch-buffer-max-mb",
        type=positive_float,
        default=64000.0,
        help="maximum shared switch buffer in Mbit; default: 64000 (64 Gb)",
    )
    parser.add_argument(
        "--seed",
        type=nonnegative_int,
        default=12345,
        help="random seed; default: 12345",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("doc/example/fluid-flow-wan-topology.generated.yaml"),
        help=(
            "output YAML path, relative to the current working directory unless absolute; "
            "default: doc/example/fluid-flow-wan-topology.generated.yaml"
        ),
    )
    parser.add_argument(
        "--name-prefix",
        default="S",
        help="switch name prefix; default: S",
    )

    args = parser.parse_args()

    if args.switches < 2:
        parser.error("--switches must be at least 2 to form a connected switch graph")

    if args.avg_switch_degree < 2.0:
        parser.error(
            "--avg-switch-degree must be at least 2.0 because the generator "
            "uses a bidirectional ring backbone"
        )

    if args.reverse_link_probability < 0.0 or args.reverse_link_probability > 1.0:
        parser.error("--reverse-link-probability must be in [0, 1]")

    if args.switch_link_min_mbps > args.switch_link_max_mbps:
        parser.error("--switch-link-min-mbps cannot exceed --switch-link-max-mbps")

    if args.terminal_link_min_mbps > args.terminal_link_max_mbps:
        parser.error("--terminal-link-min-mbps cannot exceed --terminal-link-max-mbps")

    if args.terminal_link_min_mbps <= args.switch_link_max_mbps:
        parser.error(
            "terminal access links must be faster than switch-to-switch links: "
            "require --terminal-link-min-mbps > --switch-link-max-mbps"
        )

    if args.switch_buffer_min_mb > args.switch_buffer_max_mb:
        parser.error("--switch-buffer-min-mb cannot exceed --switch-buffer-max-mb")

    return args


def rand_uniform_rounded(rng: random.Random, lo: float, hi: float) -> float:
    return round(rng.uniform(lo, hi), 3)


def format_quantity(value_in_mega: float, mega_suffix: str, giga_suffix: str) -> str:
    if value_in_mega >= 1000.0:
        return f"{value_in_mega / 1000.0:g} {giga_suffix}"
    return f"{value_in_mega:g} {mega_suffix}"


def add_link(
    links: dict[int, dict[int, float]],
    src: int,
    dst: int,
    bandwidth_mbps: float,
) -> bool:
    if src == dst:
        return False
    if dst in links[src]:
        return False
    links[src][dst] = bandwidth_mbps
    return True


def generate_links(args: argparse.Namespace, rng: random.Random) -> dict[int, dict[int, float]]:
    n = args.switches
    links: dict[int, dict[int, float]] = {i: {} for i in range(n)}

    def new_switch_bw() -> float:
        return rand_uniform_rounded(
            rng, args.switch_link_min_mbps, args.switch_link_max_mbps
        )

    # Strongly connected but asymmetric backbone.  Each direction gets its own
    # independently sampled capacity.
    for i in range(n):
        add_link(links, i, (i + 1) % n, new_switch_bw())
        add_link(links, i, (i - 1) % n, new_switch_bw())

    target_edges = max(int(round(args.switches * args.avg_switch_degree)), 2 * n)
    max_edges = n * (n - 1)
    if target_edges > max_edges:
        target_edges = max_edges

    current_edges = sum(len(v) for v in links.values())
    attempts = 0
    max_attempts = max(1000, 50 * max_edges)

    while current_edges < target_edges and attempts < max_attempts:
        attempts += 1
        src = rng.randrange(n)
        dst = rng.randrange(n)
        if src == dst:
            continue

        if add_link(links, src, dst, new_switch_bw()):
            current_edges += 1

        if current_edges >= target_edges:
            break

        if rng.random() < args.reverse_link_probability:
            if add_link(links, dst, src, new_switch_bw()):
                current_edges += 1

    if current_edges < target_edges:
        raise RuntimeError(
            f"could only generate {current_edges} directed switch links; "
            f"target was {target_edges}"
        )

    return links


def switch_name(index: int, n_switches: int, prefix: str) -> str:
    width = max(2, len(str(n_switches - 1)))
    return f"{prefix}{index:0{width}d}"


def write_topology(args: argparse.Namespace, links: dict[int, dict[int, float]]) -> None:
    rng = random.Random(args.seed + 1)
    n = args.switches
    names = [switch_name(i, n, args.name_prefix) for i in range(n)]

    args.output.parent.mkdir(parents=True, exist_ok=True)

    with args.output.open("w", encoding="utf-8") as f:
        f.write("# Generated by src/network-workloads/generate-fluid-flow-wan-topology.py\n")
        f.write(f"# switches={args.switches}\n")
        f.write(f"# terminals_per_switch={args.terminals_per_switch}\n")
        f.write(f"# avg_switch_degree={args.avg_switch_degree}\n")
        f.write(f"# seed={args.seed}\n")
        f.write("topology:\n")
        f.write("  switches:\n")

        for i in range(n):
            terminal_bw = rand_uniform_rounded(
                rng, args.terminal_link_min_mbps, args.terminal_link_max_mbps
            )
            switch_buffer = rand_uniform_rounded(
                rng, args.switch_buffer_min_mb, args.switch_buffer_max_mb
            )

            f.write(f"    {names[i]}:\n")
            f.write(f"      terminals: {args.terminals_per_switch}\n")
            terminal_bandwidth = format_quantity(terminal_bw, "Mbps", "Gbps")
            buffer_size = format_quantity(switch_buffer, "Mb", "Gb")
            f.write(f'      terminal_bandwidth: "{terminal_bandwidth}"\n')
            f.write(f'      switch_buffer: "{buffer_size}"\n')
            f.write("      connections:\n")

            for dst in sorted(links[i]):
                bandwidth = format_quantity(links[i][dst], "Mbps", "Gbps")
                f.write(f'        {names[dst]}: "{bandwidth}"\n')

            f.write("\n")


def summarize(args: argparse.Namespace, links: dict[int, dict[int, float]]) -> None:
    edge_count = sum(len(v) for v in links.values())
    degrees = [len(links[i]) for i in range(args.switches)]
    bws = [bw for edges in links.values() for bw in edges.values()]

    print(f"wrote {args.output}")
    print(f"switches: {args.switches}")
    print(f"terminals: {args.switches * args.terminals_per_switch}")
    print(f"directed switch links: {edge_count}")
    print(f"average directed switch out-degree: {edge_count / args.switches:.3f}")
    print(f"min/max directed switch out-degree: {min(degrees)} / {max(degrees)}")
    print(f"switch-link Mbps min/max: {min(bws):.3f} / {max(bws):.3f}")
    print(
        "terminal-link Mbps range: "
        f"{args.terminal_link_min_mbps:.3f} / {args.terminal_link_max_mbps:.3f}"
    )


def main() -> int:
    args = parse_args()
    rng = random.Random(args.seed)
    links = generate_links(args, rng)
    write_topology(args, links)
    summarize(args, links)
    return 0


if __name__ == "__main__":
    sys.exit(main())
