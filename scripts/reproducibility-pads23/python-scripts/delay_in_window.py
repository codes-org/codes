from __future__ import annotations

import glob
import sys
import fileinput
import pathlib
from typing import Any
import argparse

import numpy as np
import matplotlib.pyplot as plt


ndarray = np.ndarray[Any, np.dtype[np.float64]]


def collect_data_numpy(
    path: pathlib.Path | str,
    filepreffix: str,
    delimiter: str | None = None,
    dtype: Any = int
) -> tuple[list[str], np.ndarray[Any, Any]]:
    escaped_path = pathlib.Path(glob.escape(path))  # type: ignore
    stat_files = glob.glob(str(escaped_path / f"{filepreffix}-gid=*.txt"))
    if not stat_files:
        print(f"No valid `{filepreffix}` files have been found in path {path}", file=sys.stderr)
        exit(1)

    data = np.loadtxt(fileinput.input(stat_files), delimiter=delimiter, dtype=dtype,
                      comments='#')
    with open(stat_files[0], 'r') as f:
        header = f.readline()[1:].split(',')

    return header, data


def mean_and_std(array: ndarray) -> tuple[float, float, float]:
    return np.mean(array), np.std(array), float(array.shape[0])  # type: ignore


def find_mean_and_std_through_window(
    delays: ndarray,
    n_windows: int = 100,
    start_time: float = 0.0,
    end_time: float | None = None,
    end_time_col: int = 9,
    delay_col: int = 10,
) -> tuple[ndarray, ndarray, ndarray, ndarray]:

    if end_time is None:
        end_time = delays[:, end_time_col].max()

    window_time = (end_time - start_time) / n_windows
    windows = window_time * (np.arange(n_windows) + 1)
    mean_and_std_through_windows = np.zeros((n_windows, 3))
    for i in range(n_windows):
        delays_within_window = np.bitwise_and(i * window_time <= delays[:, end_time_col],
                                              delays[:, end_time_col] < (i+1) * window_time)
        if delays_within_window.sum() > 0:
            mean_and_std_through_windows[i] = mean_and_std(delays[delays_within_window, delay_col])
        else:
            mean_and_std_through_windows[i] = -1

    last_good, = np.where(mean_and_std_through_windows[:, 0] == -1)
    if last_good.size > 0:
        windows = windows[:last_good[0]]
        mean_and_std_through_windows = mean_and_std_through_windows[:last_good[0]]

    return windows, mean_and_std_through_windows[:, 0], mean_and_std_through_windows[:, 1], \
        mean_and_std_through_windows[:, 2].astype(np.int32)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--latencies', type=pathlib.Path, help='Folder to latencies',
                        required=True)
    parser.add_argument('--output', type=pathlib.Path, help='Directory to save aggregated stats',
                        required=True)
    parser.add_argument('--windows', type=int, help='Total windows to break simulation in',
                        default=100)
    parser.add_argument('--start', type=float, help='Total (virtual) simulation time',
                        required=True)
    parser.add_argument('--end', type=float, help='Total (virtual) simulation time',
                        required=True)
    args = parser.parse_args()

    plotting = False
    computing = True
    use_cython = True

    loading = not computing
    end_time = args.end
    n_windows = args.windows

    # Hardcoded values for 72-node dragonfly network
    dist_type = 'all'  # options: all, same_router, same_group, other_group

    out_file_name = f"{args.output}.npz"

    if computing:
        if use_cython:
            import pyximport; pyximport.install(language_level='3str')  # noqa: E702
            from file_read_cython.read_mean_std_from_file import load_mean_and_std_through_window

            windows, n_samples, samples = load_mean_and_std_through_window(
                str(args.latencies), args.start, args.end, num_windows=args.windows)
            means, stds = samples[:, 0], samples[:, 1]

        else:
            # Columns within the csv file that matter to us
            header, delays = collect_data_numpy(
                args.latencies, 'packets-delay', delimiter=',',
                dtype=np.dtype('float'))
            end_time_col = header.index('end')
            delay_col = header.index('latency')

            # Delays distributions
            if dist_type != 'all':
                delays_same_router = (delays[:, 0] // 2) == (delays[:, 1] // 2)
                delays_same_group = np.bitwise_xor(
                    (delays[:, 0] // 8) == (delays[:, 1] // 8),
                    delays_same_router)
                delays_out_group = (delays[:, 0] // 8) != (delays[:, 1] // 8)

                # Selecting which distribution to display
                if dist_type == 'same_router':
                    distribution = delays_same_router
                elif dist_type == 'same_group':
                    distribution = delays_same_group
                elif dist_type == 'other_group':
                    distribution = delays_out_group

            # Computing windowed mean and stds + plotting
            windows, means, stds, n_samples = find_mean_and_std_through_window(
                delays if dist_type == 'all' else delays[distribution],
                n_windows=n_windows, end_time_col=end_time_col,
                delay_col=delay_col, end_time=end_time)

        # Save
        np.savez(out_file_name,
                 windows=windows, means=means, stds=stds, n_samples=n_samples)

    if loading:
        data = np.load(out_file_name)
        windows, means, stds = data['windows'], data['means'], data['stds']

    if plotting:
        plt.errorbar(windows, means, yerr=.2*stds)
        plt.show()  # type: ignore
