from __future__ import annotations

import argparse
import pathlib

import numpy as np
import matplotlib.pyplot as plt
import matplotlib
from matplotlib.ticker import EngFormatter


time_formatter_ns = EngFormatter()
time_formatter_ns.ENG_PREFIXES = {0: 'ns', 3: 'us', 6: 'ms', 9: 's'}


if True and __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--latencies', type=pathlib.Path, help='Folder with condensed latencies',
                        required=True)
    parser.add_argument('--output', type=pathlib.Path, help='Name of output figure',
                        required=True)
    args = parser.parse_args()

    latex = True

    if latex:
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

    std_factor = 0.2

    fig, ax = plt.subplots(figsize=(7, 3.8))
    ax.vlines = ax.vlines([2e6, 3e6, 8e6], -3e3, 125e3, color='#AAA', ls='-')
    ax.vlines.set_clip_on(False)

    arrow_color = {'arrowprops': dict(arrowstyle="->", color='#AAA'), 'color': '#333'}
    ax.annotate("", xy=(1.95e6, 80e3), xytext=(1.0e6, 98e3), **arrow_color)
    ax.annotate("switch", xy=(3.1e6, 118e3), xytext=(4.8e6, 105e3), **arrow_color)
    ax.annotate("", xy=(7.9e6, 118e3), xytext=(6.0e6, 110e3), **arrow_color)
    ax.text(1.9e6, 1e5, "start\ntracking", color='#333', ha='right')

    # plt.errorbar(windows_hf, means_hf, yerr=std_factor*stds_hf)
    # plt.errorbar(windows_hybrid, means_hybrid, yerr=std_factor*stds_hybrid)
    # plt.errorbar(windows_hybrid_lite, means_hybrid_lite,
    #              yerr=std_factor*stds_hybrid_lite)
    ax.plot(windows_hf, means_hf, label='high-fidelity only')
    ax.fill_between(windows_hf,
                    means_hf - std_factor*stds_hybrid,
                    means_hf + std_factor*stds_hybrid,
                    color='#00F5')
    ax.plot(windows_hybrid_lite, means_hybrid_lite, label='hybrid-lite')
    ax.fill_between(windows_hybrid_lite,
                    means_hybrid_lite - std_factor*stds_hybrid,
                    means_hybrid_lite + std_factor*stds_hybrid,
                    color='#F005')
    ax.plot(windows_hybrid, means_hybrid, label='hybrid')
    ax.fill_between(windows_hybrid,
                    means_hybrid - std_factor*stds_hybrid,
                    means_hybrid + std_factor*stds_hybrid,
                    color='#0F05')

    # ax.text(2e6, 125e3, "start latency tracking", color='#333', rotation=40,
    #         rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')
    # ax.text(3e6, 125e3, "switch to surrogate", color='#333', rotation=40,
    #         rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')
    # ax.text(8e6, 130e3, "switch to\nhigh-definition", color='#333', rotation=40,
    #         rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')

    ax.set_xlabel('Virtual time')
    ax.set_ylabel('Average Packet Latency')
    ax.set_ylim(0, 122e3)
    ax.legend(bbox_to_anchor=(.54, .02), loc='lower center', borderaxespad=0)
    ax.yaxis.set_major_formatter(time_formatter_ns)
    ax.xaxis.set_major_formatter(time_formatter_ns)

    n = means_hf[80:].shape[0]
    mse_hybrid_lite = \
        np.sum((means_hf[80:] - means_hybrid_lite[80:])**2) / n
    mse_hybrid = \
        np.sum((means_hf[80:] - means_hybrid[80:])**2) / n
    print("Mean squared error (MSE) for hybrid:", mse_hybrid, "ns^2")
    print("Mean squared error (MSE) for hybrid-lite:", mse_hybrid_lite, "ns^2")

    if latex:
        plt.tight_layout()
        plt.savefig(f'{args.output}.pgf', bbox_inches='tight')
        plt.savefig(f'{args.output}.pdf', bbox_inches='tight')
    else:
        plt.show()


if False and __name__ == '__main__':
    data_high_fidelity = \
        np.load("data/vanilla-synthetic1-100ms_windowed_packet_latency_all.npz")
    data_hybrid = \
        np.load("data/surrogate-freezing-synthetic1-100ms_windowed_packet_latency_all.npz")
    data_hybrid_lite = \
        np.load("data/surrogate-nonfrozen-synthetic1-100ms_windowed_packet_latency_all.npz")

    windows_hf, means_hf, stds_hf = \
        data_high_fidelity['windows'], data_high_fidelity['means'], data_high_fidelity['stds']
    windows_hybrid, means_hybrid, stds_hybrid = \
        data_hybrid['windows'], data_hybrid['means'], data_hybrid['stds']
    windows_hybrid_lite, means_hybrid_lite, stds_hybrid_lite = \
        data_hybrid_lite['windows'], data_hybrid_lite['means'], data_hybrid_lite['stds']

    assert np.all(windows_hf == windows_hybrid) \
        and np.all(windows_hybrid_lite == windows_hybrid)

    std_factor = 0.2

    fig, ax = plt.subplots(figsize=(7, 6))
    # ax.vlines = ax.vlines([2e6, 3e6, 8e6], -3e3, 125e3, color='#AAA', ls='-')
    # ax.vlines.set_clip_on(False)

    ax.plot(windows_hf, means_hf, label='high-fidelity only')
    ax.fill_between(windows_hf,
                    means_hf - std_factor*stds_hybrid,
                    means_hf + std_factor*stds_hybrid,
                    color='#00F5')
    ax.plot(windows_hybrid_lite, means_hybrid_lite, label='hybrid-lite')
    ax.fill_between(windows_hybrid_lite,
                    means_hybrid_lite - std_factor*stds_hybrid,
                    means_hybrid_lite + std_factor*stds_hybrid,
                    color='#F005')
    ax.plot(windows_hybrid, means_hybrid, label='hybrid')
    ax.fill_between(windows_hybrid,
                    means_hybrid - std_factor*stds_hybrid,
                    means_hybrid + std_factor*stds_hybrid,
                    color='#0F05')

    ax.yaxis.set_major_formatter(time_formatter_ns)
    ax.xaxis.set_major_formatter(time_formatter_ns)

    n = means_hf[90:].shape[0]
    mse_hybrid_lite = \
        np.sum((means_hf[90:] - means_hybrid_lite[90:])**2) / n
    mse_hybrid = \
        np.sum((means_hf[90:] - means_hybrid[90:])**2) / n
    print("Mean squared error (MSE) for hybrid:", mse_hybrid, "ns^2")
    print("Mean squared error (MSE) for hybrid-lite:", mse_hybrid_lite, "ns^2")

    plt.show()


if False and __name__ == '__main__':
    latex = True

    if latex:
        matplotlib.use("pgf")
        matplotlib.rcParams.update({
            "pgf.texsystem": "pdflatex",
            'font.family': 'serif',
            'font.size': 16,
            'text.usetex': True,
            'pgf.rcfonts': False,
        })

    data_high_fidelity = \
        np.load("data/vanilla-ping-pong-10ms_windowed_packet_latency_all.npz")
    data_hybrid = \
        np.load("data/surrogate-freezing-ping-pong-10ms_windowed_packet_latency_all.npz")
    data_hybrid_lite = \
        np.load("data/surrogate-nonfrozen-ping-pong-10ms_windowed_packet_latency_all.npz")

    windows_hf, means_hf, stds_hf = \
        data_high_fidelity['windows'], data_high_fidelity['means'], data_high_fidelity['stds']
    windows_hybrid, means_hybrid, stds_hybrid = \
        data_hybrid['windows'], data_hybrid['means'], data_hybrid['stds']
    windows_hybrid_lite, means_hybrid_lite, stds_hybrid_lite = \
        data_hybrid_lite['windows'], data_hybrid_lite['means'], data_hybrid_lite['stds']

    assert np.all(windows_hf == windows_hybrid) \
        and np.all(windows_hybrid_lite == windows_hybrid)

    std_factor = 0.2

    fig, ax = plt.subplots(figsize=(7, 3.8))
    ax.vlines = ax.vlines([0, 1e6, 8e6], 2.55e3, 4.45e3, color='#AAA', ls='-')
    ax.vlines.set_clip_on(False)

    arrow_color = {'arrowprops': dict(arrowstyle="->", color='#AAA'), 'color': '#333'}
    ax.annotate("", xy=(0.1e6, 2.65e3), xytext=(1.5e6, 2.95e3), **arrow_color)
    ax.text(1.5e6, 2.95e3, "start latency tracking", color='#333', ha='left')
    ax.annotate("switch", xy=(1.1e6, 2.65e3), xytext=(4.0e6, 2.75e3), **arrow_color)
    ax.annotate("", xy=(7.9e6, 2.65e3), xytext=(5.2e6, 2.75e3), **arrow_color)

    ax.plot(windows_hf, means_hf, label='high-fidelity only')
    ax.fill_between(windows_hf,
                    means_hf - std_factor*stds_hybrid,
                    means_hf + std_factor*stds_hybrid,
                    color='#00F5')
    ax.plot(windows_hybrid_lite, means_hybrid_lite, label='hybrid-lite')
    ax.fill_between(windows_hybrid_lite,
                    means_hybrid_lite - std_factor*stds_hybrid,
                    means_hybrid_lite + std_factor*stds_hybrid,
                    color='#F005')
    ax.plot(windows_hybrid, means_hybrid, label='hybrid')
    ax.fill_between(windows_hybrid,
                    means_hybrid - std_factor*stds_hybrid,
                    means_hybrid + std_factor*stds_hybrid,
                    color='#0F05')

    # plt.text(0, 4.5e3, "start latency tracking", color='#333', rotation=40,
    #          rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')
    # plt.text(1e6, 4.5e3, "switch to surrogate", color='#333', rotation=40,
    #          rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')
    # plt.text(8e6, 4.5e3, "switch to\nhigh-definition", color='#333', rotation=40,
    #          rotation_mode='anchor', horizontalalignment='left', verticalalignment='center')

    ax.set_xlabel('Virtual time')
    ax.set_ylabel('Average Packet Latency')
    ax.set_ylim(2.6e3, 4.4e3)
    ax.legend(bbox_to_anchor=(.50, .28), loc='lower center', borderaxespad=0)
    ax.yaxis.set_major_formatter(time_formatter_ns)
    ax.xaxis.set_major_formatter(time_formatter_ns)

    n = means_hf[90:].shape[0]
    mse_hybrid_lite = \
        np.sum((means_hf[90:] - means_hybrid_lite[90:])**2) / n
    mse_hybrid = \
        np.sum((means_hf[90:] - means_hybrid[90:])**2) / n
    print("Mean squared error (MSE) for hybrid:", mse_hybrid, "ns^2")
    print("Mean squared error (MSE) for hybrid-lite:", mse_hybrid_lite, "ns^2")

    if latex:
        plt.tight_layout()
        plt.savefig('figures/windowed-delay-ping-pong-10ms.pgf', bbox_inches='tight')
        plt.savefig('figures/windowed-delay-ping-pong-10ms.pdf', bbox_inches='tight')
    else:
        plt.show()
