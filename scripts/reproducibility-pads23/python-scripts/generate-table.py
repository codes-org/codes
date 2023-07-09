from __future__ import annotations

import argparse
import pathlib
from subprocess import check_output
from glob import glob
import csv

import numpy as np


def determine_mse(
    condensed: pathlib.Path, cut_off: int = 80, check_last: bool = True
) -> tuple[float, float]:
    """Returns in us**2 (not ns**2)"""
    data_high_fidelity = np.load(f"{condensed}/packet_latency-high-fidelity.npz")
    data_hybrid = np.load(f"{condensed}/packet_latency-hybrid.npz")
    data_hybrid_lite = np.load(f"{condensed}/packet_latency-hybrid-lite.npz")

    windows_hf, means_hf = data_high_fidelity['windows'], data_high_fidelity['means']
    windows_hybrid, means_hybrid = data_hybrid['windows'], data_hybrid['means']
    means_hybrid_lite = data_hybrid_lite['means']

    assert np.all(windows_hf == windows_hybrid)
    if check_last:
        n_windows = windows_hf.shape[0]
        means_hybrid_lite = means_hybrid_lite[:n_windows]

    n = means_hf[cut_off:].shape[0]
    mse_hybrid_lite = np.sum((means_hf[cut_off:] - means_hybrid_lite[cut_off:])**2) / n
    mse_hybrid = np.sum((means_hf[cut_off:] - means_hybrid[cut_off:])**2) / n

    return mse_hybrid / 1e6, mse_hybrid_lite / 1e6


def get_runtimes(path: pathlib.Path) -> tuple[float, float, float]:
    with open(path, newline='') as f:
        reader = csv.reader(f)
        csv_file = [row for row in reader]

    assert len(csv_file) == 4
    assert csv_file[0][8] == 'runtime'
    return float(csv_file[1][8]), float(csv_file[2][8]), float(csv_file[3][8])


def get_total_packets(latencies_dir: pathlib.Path) -> int:
    out = check_output(
        ['wc', '-l', '--total=always'] + glob(str(latencies_dir / "packets-delay-*"))
    ).split()
    assert out[-1] == b'total'
    return int(out[-2])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--folder-10ms', type=pathlib.Path, required=True,
                        help='Execution folder for 10 ms')
    parser.add_argument('--folder-100ms', type=pathlib.Path, required=True,
                        help='Execution folder for 100 ms')
    args = parser.parse_args()

    packets_hf = get_total_packets(args.folder_10ms / 'high-fidelity' / 'packet-latency-trace')
    packets_hybrid = get_total_packets(args.folder_10ms / 'hybrid' / 'packet-latency-trace')
    packets_hybrid_lite = get_total_packets(
        args.folder_10ms / 'hybrid-lite' / 'packet-latency-trace')
    throughput_hf = packets_hf * 1024 / 1024**3 * 100
    throughput_hybrid = packets_hybrid * 1024 / 1024**3 * 100
    throughput_hybrid_lite = packets_hybrid_lite * 1024 / 1024**3 * 100
    runtime_hf, runtime_hybrid, runtime_hybrid_lite = get_runtimes(args.folder_10ms / 'ross.csv')
    throughput_hybrid_dis = (throughput_hybrid / throughput_hf - 1) * 100
    throughput_hybrid_lite_dis = (throughput_hybrid_lite / throughput_hf - 1) * 100

    mse_hybrid, mse_hybrid_lite = determine_mse(args.folder_10ms / 'condensed')
    print("10 ms Results")
    print("Throughput (GB/s) high-fidelity:", throughput_hf)
    print("Throughput (GB/s) hybrid:", throughput_hybrid)
    print("Throughput (GB/s) hybrid-lite:", throughput_hybrid_lite)
    print("Throughput (%) hybrid discrepancy:", throughput_hybrid_dis)
    print("Throughput (%) hybrid-lite discrepancy:", throughput_hybrid_lite_dis)
    print("Runtime (s) high-fidelity:", runtime_hf)
    print("Runtime (s) hybrid:", runtime_hybrid)
    print("Runtime (s) hybrid-lite:", runtime_hybrid_lite)
    print("Mean squared error (MSE) for hybrid:", mse_hybrid, "ns^2")
    print("Mean squared error (MSE) for hybrid-lite:", mse_hybrid_lite, "ns^2")
    print()

    packets_hf = get_total_packets(args.folder_100ms / 'high-fidelity' / 'packet-latency-trace')
    packets_hybrid = get_total_packets(args.folder_100ms / 'hybrid' / 'packet-latency-trace')
    packets_hybrid_lite = get_total_packets(
        args.folder_100ms / 'hybrid-lite' / 'packet-latency-trace')
    throughput_hf = packets_hf * 1024 / 1024**3 * 10
    throughput_hybrid = packets_hybrid * 1024 / 1024**3 * 10
    throughput_hybrid_lite = packets_hybrid_lite * 1024 / 1024**3 * 10
    runtime_hf, runtime_hybrid, runtime_hybrid_lite = get_runtimes(args.folder_100ms / 'ross.csv')
    throughput_hybrid_dis = (throughput_hybrid / throughput_hf - 1) * 100
    throughput_hybrid_lite_dis = (throughput_hybrid_lite / throughput_hf - 1) * 100

    print("100 ms Results")
    print("Throughput (GB/s) high-fidelity:", throughput_hf)
    print("Throughput (GB/s) hybrid:", throughput_hybrid)
    print("Throughput (GB/s) hybrid-lite:", throughput_hybrid_lite)
    print("Throughput (%) hybrid discrepancy:", throughput_hybrid_dis)
    print("Throughput (%) hybrid-lite discrepancy:", throughput_hybrid_lite_dis)
    print("Runtime (s) high-fidelity:", runtime_hf)
    print("Runtime (s) hybrid:", runtime_hybrid)
    print("Runtime (s) hybrid-lite:", runtime_hybrid_lite)
    mse_hybrid, mse_hybrid_lite = determine_mse(args.folder_100ms / 'condensed',
                                                cut_off=90, check_last=False)
    print("Mean squared error (MSE) for hybrid:", mse_hybrid, "ns^2")
    print("Mean squared error (MSE) for hybrid-lite:", mse_hybrid_lite, "ns^2")
