import numpy as np
import matplotlib.pyplot as plt
import matplotlib
from matplotlib.ticker import EngFormatter
import pathlib
import argparse

from typing import Any
array_type = np.ndarray[Any, Any]


time_formatter_ns = EngFormatter()
time_formatter_ns.ENG_PREFIXES = {0: 'ns', 3: 'us', 6: 'ms', 9: 's'}
bytes_formater = EngFormatter(unit='B')


def load_aggregated_utilization(filename: str | pathlib.Path) -> tuple[array_type, array_type]:
    port_utilization = np.loadtxt(filename, delimiter=',', dtype=float, skiprows=1)

    # finding all snapshot timestamps
    timestamps = np.unique(port_utilization[:, 0])
    assert len(timestamps.shape) == 1

    # Finding total utilization per snapshot
    total_utilization = np.zeros_like(timestamps)
    for i, ts in enumerate(timestamps):
        total_utilization[i] = port_utilization[port_utilization[:, 0] == ts, 2:].sum()

    return timestamps, total_utilization


if True and __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--experiment-folder', type=pathlib.Path,
                        help='Folder where experiment was run',
                        required=True)
    parser.add_argument('--output', type=pathlib.Path, help='Name of output figure',
                        required=True)
    args = parser.parse_args()

    latex = True
    dir_data = args.experiment_folder
    # dir_data = pathlib.Path('data/synthetic1')
    cut1 = 30
    cut2 = 79

    if latex:
        matplotlib.use("pgf")
        matplotlib.rcParams.update({
            "pgf.texsystem": "pdflatex",
            'font.family': 'serif',
            'font.size': 16,
            'text.usetex': True,
            'pgf.rcfonts': False,
        })

    ts1, utilization_hf = load_aggregated_utilization(
        dir_data / "high-fidelity" / "codes-output" / "dragonfly-snapshots.csv")
    ts2, utilization_hybrid = load_aggregated_utilization(
        dir_data / "hybrid" / "codes-output" / "dragonfly-snapshots.csv")
    ts3, utilization_hybrid_lite = load_aggregated_utilization(
        dir_data / "hybrid-lite" / "codes-output" / "dragonfly-snapshots.csv")

    # plotting
    fig, ax = plt.subplots(figsize=(7, 3.8))
    vlines = ax.vlines([2e6, 3e6, 8e6], -0.4e6, 7.15e6, color='#AAA', ls='-')
    vlines.set_clip_on(False)

    arrow_color = {'arrowprops': dict(arrowstyle="->", color='#AAA'), 'color': '#333'}
    ax.annotate("", xy=(2.1e6, 0e6), xytext=(3.5e6, 1.1e6), **arrow_color)
    ax.annotate("switch", xy=(3.1e6, 0.1e6), xytext=(4.8e6, 0.5e6), **arrow_color)
    ax.annotate("", xy=(7.9e6, 0.1e6), xytext=(6.0e6, 0.5e6), **arrow_color)
    ax.text(3.5e6, 1.1e6, "start latency tracking", color='#333', ha='left')

    ax.plot(ts1, utilization_hf, label="high-fidelity", color='blue')

    ax.plot(ts3[:cut1], utilization_hybrid_lite[:cut1],
            label="hybrid-lite", color='red')
    ax.plot(ts3[cut1-1:cut2+1], utilization_hybrid_lite[cut1-1:cut2+1],
            color='red', ls='--')
    ax.plot(ts3[cut2:], utilization_hybrid_lite[cut2:], color='red')

    ax.plot(ts2[:cut1], utilization_hybrid[:cut1], label="hybrid",
            color='green')
    ax.plot(ts2[cut1-1:cut2+1], utilization_hybrid[cut1-1:cut2+1], color='green', ls='--')
    ax.plot(ts2[cut2:], utilization_hybrid[cut2:], color='green')

    # ax.text(2e6, 7.4e6, "start latency tracking", color='#333', rotation=40,
    #         rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')
    # ax.text(3e6, 7.4e6, "switch to surrogate", color='#333', rotation=40,
    #         rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')
    # ax.text(8e6, 7.4e6, "switch to\nhigh-definition", color='#333', rotation=40,
    #         rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')

    ax.set_xlabel('Virtual time')
    ax.set_ylabel('Total Buffer Port Occupancy')
    ax.set_ylim(-0.2e6, 6.9e6)
    ax.legend(bbox_to_anchor=(.5, .4), loc='lower center', borderaxespad=0)
    ax.xaxis.set_major_formatter(time_formatter_ns)
    ax.yaxis.set_major_formatter(bytes_formater)

    if latex:
        plt.tight_layout()
        plt.savefig(f'{args.output}.pgf', bbox_inches='tight')
        plt.savefig(f'{args.output}.pdf', bbox_inches='tight')
    else:
        plt.show()


if False and __name__ == '__main__':
    latex = True
    dir_data = pathlib.Path('data/ping-pong')
    cut1 = 10
    cut2 = 79

    if latex:
        matplotlib.use("pgf")
        matplotlib.rcParams.update({
            "pgf.texsystem": "pdflatex",
            'font.family': 'serif',
            'font.size': 16,
            'text.usetex': True,
            'pgf.rcfonts': False,
        })

    ts1, utilization_hf = \
        load_aggregated_utilization(dir_data / "router-snapshots-vanilla.csv")
    ts2, utilization_hybrid = \
        load_aggregated_utilization(dir_data / "router-snapshots-surrogate-freezing.csv")
    ts3, utilization_hybrid_lite = \
        load_aggregated_utilization(dir_data / "router-snapshots-surrogate-nonfrozen.csv")

    # plotting
    fig, ax = plt.subplots(figsize=(7, 3.8))
    vlines = ax.vlines([0, 1e6, 8e6], -0.05e5, 1.11e5, color='#AAA', ls='-')
    vlines.set_clip_on(False)

    arrow_color = {'arrowprops': dict(arrowstyle="->", color='#AAA'), 'color': '#333'}
    ax.annotate("", xy=(0.1e6, 0e5), xytext=(2e6, 0.16e5), **arrow_color)
    ax.annotate("switch", xy=(1.1e6, 0.0e5), xytext=(4.8e6, 0.06e5), **arrow_color)
    ax.annotate("", xy=(7.9e6, 0.0e5), xytext=(6.0e6, 0.06e5), **arrow_color)
    ax.text(2e6, 0.16e5, "start latency tracking", color='#333', ha='left')

    ax.plot(ts1, utilization_hf, label="high-fidelity", color='blue')

    ax.plot(ts3[:cut1], utilization_hybrid_lite[:cut1],
            label="hybrid-lite", color='red')
    ax.plot(ts3[cut1-1:cut2+1], utilization_hybrid_lite[cut1-1:cut2+1],
            color='red', ls='--')
    ax.plot(ts3[cut2:], utilization_hybrid_lite[cut2:], color='red')

    ax.plot(ts2[:cut1], utilization_hybrid[:cut1], label="hybrid",
            color='green')
    ax.plot(ts2[cut1-1:cut2+1], utilization_hybrid[cut1-1:cut2+1], color='green', ls='--')
    ax.plot(ts2[cut2:], utilization_hybrid[cut2:], color='green')

    # ax.text(0, 1.15e5, "start latency tracking", color='#333', rotation=40,
    #         rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')
    # ax.text(1e6, 1.15e5, "switch to surrogate", color='#333', rotation=40,
    #         rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')
    # ax.text(8e6, 1.15e5, "switch to\nhigh-definition", color='#333', rotation=40,
    #         rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')

    ax.set_xlabel('Virtual time')
    ax.set_ylabel('Total Buffer Port Occupancy')
    ax.set_ylim(-0.02e5, 1.08e5)
    ax.legend(bbox_to_anchor=(.48, .28), loc='lower center', borderaxespad=0)
    ax.xaxis.set_major_formatter(time_formatter_ns)
    ax.yaxis.set_major_formatter(bytes_formater)

    if latex:
        plt.tight_layout()
        plt.savefig('figures/port-occupancy-ping-pong.pgf', bbox_inches='tight')
        plt.savefig('figures/port-occupancy-ping-pong.pdf', bbox_inches='tight')
    else:
        plt.show()
