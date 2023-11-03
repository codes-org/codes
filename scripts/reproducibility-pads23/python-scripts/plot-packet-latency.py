from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np
import matplotlib.pyplot as plt
import matplotlib
from matplotlib.ticker import EngFormatter

from delay_in_window import collect_data_numpy, find_mean_and_std_through_window


time_formatter_ns = EngFormatter()
time_formatter_ns.ENG_PREFIXES = {0: 'ns', 3: 'us', 6: 'ms', 9: 's'}


if __name__ == '__main__':
    this_binary = sys.argv[0]
    commands = {
        'plotfromraw': 'Generates a single packet-latency plot given the raw latency data',
        'plotfromzip': 'Generates a single packet-latency plot given a zipped file (NPZ).'
                       ' (npz file geterated by delay_in_window.py)',
        'pads23': 'Generates plot that appears on PADS23 paper'
    }
    parser = argparse.ArgumentParser(
        usage=f'{this_binary} <command> [<args>]\n\n'
        'The available commands are:\n'
        + '\n'.join(f'  {cmd}\t {desc}' for cmd, desc in commands.items()))
    parser.add_argument('command', help='Subcommand to run')
    main_args = parser.parse_args(sys.argv[1:2])

    if main_args.command not in commands:
        print("Unrecognized command:", main_args.command, file=sys.stderr)
        exit(1)


if main_args.command == 'plotfromraw':
    parser = argparse.ArgumentParser()
    parser.add_argument('--latencies-dir', type=pathlib.Path, required=True,
                        help='Folder with raw latency data')
    parser.add_argument('--windows', type=int, help='Total windows to break simulation in',
                        default=100)
    parser.add_argument('--end', type=float, help='Total (virtual) simulation time',
                        required=True)
    parser.add_argument('--std-factor', type=float, default=0.2,
                        help='Size of variance to show as an std factor')
    args = parser.parse_args(sys.argv[2:])

    std_factor = args.std_factor

    header, delays = collect_data_numpy(args.latencies_dir, 'packets-delay', delimiter=',',
                                        dtype=np.dtype('float'))

    # Cleaning data
    next_packet_delay_col = header.index('next_packet_delay')
    delays = delays[delays[:, next_packet_delay_col] > 0]

    delay_col = header.index('latency')
    windows, means, stds = find_mean_and_std_through_window(
        delays, n_windows=args.windows, delay_col=delay_col, end_time=args.end)

    fig, ax = plt.subplots()

    # plt.errorbar(windows, means, yerr=std_factor*stds)
    ax.plot(windows, means, label='high-fidelity only')
    ax.fill_between(windows,
                    means - std_factor*stds,
                    means + std_factor*stds,
                    color='#00F5')

    ax.set_xlabel('Virtual time')
    ax.set_ylabel('Average Packet Latency')
    ax.yaxis.set_major_formatter(time_formatter_ns)
    ax.xaxis.set_major_formatter(time_formatter_ns)

    plt.show()  # type: ignore


if main_args.command == 'plotfromzip':
    parser = argparse.ArgumentParser()
    parser.add_argument('--latencies', type=pathlib.Path, required=True,
                        help='NPZ file containing packet-latency data')
    parser.add_argument('--std-factor', type=float, default=0.2,
                        help='Size of variance to show as an std factor')
    args = parser.parse_args(sys.argv[2:])

    std_factor = args.std_factor

    data_npz = np.load(args.latencies)
    windows, means, stds = data_npz['windows'], data_npz['means'], data_npz['stds']

    fig, ax = plt.subplots()

    # plt.errorbar(windows, means, yerr=std_factor*stds)
    ax.plot(windows, means, label='high-fidelity only')
    ax.fill_between(windows,
                    means - std_factor*stds,
                    means + std_factor*stds,
                    color='#00F5')

    ax.set_xlabel('Virtual time')
    ax.set_ylabel('Average Packet Latency')
    ax.yaxis.set_major_formatter(time_formatter_ns)
    ax.xaxis.set_major_formatter(time_formatter_ns)

    plt.show()  # type: ignore


if main_args.command == 'pads23':
    parser = argparse.ArgumentParser()
    parser.add_argument('--latencies', type=pathlib.Path, help='Folder with condensed latencies',
                        required=True)
    parser.add_argument('--output', type=pathlib.Path, help='Name of output figure',
                        default=None)
    parser.add_argument('--std-factor', type=float, default=0.2,
                        help='Size of variance to show as an std factor')
    parser.add_argument('--started-tracking', type=float, default=2e6)
    parser.add_argument('--switch', type=float, default=3e6)
    parser.add_argument('--switch-back', type=float, default=8e6)
    args = parser.parse_args(sys.argv[2:])

    std_factor = args.std_factor

    if args.output:
        matplotlib.use("pgf")
        matplotlib.rcParams.update({
            "pgf.texsystem": "pdflatex",
            'font.family': 'serif',
            'font.size': 16,
            'text.usetex': True,
            'pgf.rcfonts': False,
        })

    data_high_fidelity = np.load(f"{args.latencies}/packet_latency-high-fidelity.npz")
    data_hybrid = np.load(f"{args.latencies}/packet_latency-hybrid.npz")
    data_hybrid_lite = np.load(f"{args.latencies}/packet_latency-hybrid-lite.npz")

    windows_hf, means_hf, stds_hf = \
        data_high_fidelity['windows'], data_high_fidelity['means'], data_high_fidelity['stds']
    windows_hybrid, means_hybrid, stds_hybrid = \
        data_hybrid['windows'], data_hybrid['means'], data_hybrid['stds']
    windows_hybrid_lite, means_hybrid_lite, stds_hybrid_lite = \
        data_hybrid_lite['windows'], data_hybrid_lite['means'], data_hybrid_lite['stds']

    assert np.all(windows_hf == windows_hybrid)
    n_windows = windows_hf.shape[0]
    windows_hybrid_lite = windows_hybrid_lite[:n_windows]
    means_hybrid_lite = means_hybrid_lite[:n_windows]
    stds_hybrid_lite = stds_hybrid_lite[:n_windows]
    assert np.all(windows_hybrid_lite == windows_hybrid)

    fig, ax = plt.subplots(figsize=(7, 3.8))

    # plt.errorbar(windows_hf, means_hf, yerr=std_factor*stds_hf)
    # plt.errorbar(windows_hybrid, means_hybrid, yerr=std_factor*stds_hybrid)
    # plt.errorbar(windows_hybrid_lite, means_hybrid_lite,
    #              yerr=std_factor*stds_hybrid_lite)
    ax.plot(windows_hf, means_hf, label='high-fidelity only')
    ax.fill_between(windows_hf,
                    means_hf - std_factor*stds_hf,
                    means_hf + std_factor*stds_hf,
                    color='#00F5')
    ax.plot(windows_hybrid_lite, means_hybrid_lite, label='hybrid-lite')
    ax.fill_between(windows_hybrid_lite,
                    means_hybrid_lite - std_factor*stds_hybrid,
                    means_hybrid_lite + std_factor*stds_hybrid,
                    color='#F005')
    ax.plot(windows_hybrid, means_hybrid, label='hybrid')
    ax.fill_between(windows_hybrid,
                    means_hybrid - std_factor*stds_hybrid_lite,
                    means_hybrid + std_factor*stds_hybrid_lite,
                    color='#0F05')

    height_plot = ax.get_ylim()[1]
    ax.vlines = ax.vlines([args.started_tracking, args.switch, args.switch_back],
                          -3e3, height_plot, color='#AAA', ls='-')
    ax.vlines.set_clip_on(False)

    middle = (args.switch + args.switch_back) / 2
    arrow_color = {'arrowprops': dict(arrowstyle="->", color='#AAA'), 'color': '#333'}
    ax.annotate("", xy=(args.started_tracking * .95, 80e3),
                xytext=(args.started_tracking * .6, 98e3), **arrow_color)
    ax.annotate("switch", xy=(args.switch*1.04, 118e3),
                xytext=(middle, 105e3), **arrow_color)
    ax.annotate("", xy=(args.switch_back * 0.96, 118e3),
                xytext=(middle, 110e3), **arrow_color)
    ax.text(args.started_tracking * .9, 1e5, "start\ntracking", color='#333', ha='right')

    ax.text(args.started_tracking, height_plot, "start latency tracking", color='#333', rotation=40,
            rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')
    ax.text(args.switch, height_plot, "switch to surrogate", color='#333', rotation=40,
            rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')
    ax.text(args.switch_back, 1.03 * height_plot, "switch to\nhigh-definition", color='#333',
            rotation=40, rotation_mode='anchor', horizontalalignment='left',
            verticalalignment='center')

    ax.set_xlabel('Virtual time')
    ax.set_ylabel('Average Packet Latency')
    # ax.set_ylim(0, 122e3)
    # ax.legend(bbox_to_anchor=(.54, .02), loc='lower center', borderaxespad=0)
    ax.yaxis.set_major_formatter(time_formatter_ns)
    ax.xaxis.set_major_formatter(time_formatter_ns)

    n = means_hf[80:].shape[0]
    mse_hybrid_lite = \
        np.sum((means_hf[80:] - means_hybrid_lite[80:])**2) / n
    mse_hybrid = \
        np.sum((means_hf[80:] - means_hybrid[80:])**2) / n
    print("Mean squared error (MSE) for hybrid:", mse_hybrid, "ns^2")
    print("Mean squared error (MSE) for hybrid-lite:", mse_hybrid_lite, "ns^2")

    if args.output:
        plt.tight_layout()
        plt.savefig(f'{args.output}.pgf', bbox_inches='tight')
        plt.savefig(f'{args.output}.pdf', bbox_inches='tight')
    else:
        plt.show()  # type: ignore
