from __future__ import annotations

import glob
import sys
import fileinput
import pathlib
import argparse
from enum import Enum
import typing as t

import numpy as np
import matplotlib.pyplot as plt


ndarray: t.TypeAlias = 'np.ndarray[t.Any, np.dtype[np.float64]]'


def collect_data_numpy(
    path: pathlib.Path | str,
    filepreffix: str,
    delimiter: str | None = None,
    dtype: t.Any = int
) -> tuple[list[str], np.ndarray[t.Any, t.Any]]:
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


class SrcDestRelationship(Enum):
    Any = 0
    SameRouter = 1
    SameGroup = 2
    DifferentGroup = 3


def break_delay_data_into(
    delays: np.ndarray[t.Any, t.Any],
    src_dest_rel: SrcDestRelationship,
    nodes_per_router: int = 2,
    nodes_per_group: int = 8
) -> np.ndarray[t.Any, t.Any]:
    if src_dest_rel == SrcDestRelationship.Any:
        return delays

    elif src_dest_rel == SrcDestRelationship.DifferentGroup:
        delays_out_group = (delays[:, 0] // nodes_per_group) != (delays[:, 1] // nodes_per_group)
        return delays[delays_out_group]  # type: ignore

    else:

        delays_same_router = \
            (delays[:, 0] // nodes_per_router) == (delays[:, 1] // nodes_per_router)

        if src_dest_rel == SrcDestRelationship.SameRouter:
            return delays[delays_same_router]  # type: ignore

        else:
            assert src_dest_rel == SrcDestRelationship.SameGroup

            delays_same_group = np.bitwise_xor(
                (delays[:, 0] // nodes_per_group) == (delays[:, 1] // nodes_per_group),
                delays_same_router)

            return delays[delays_same_group]  # type: ignore


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
    # The following aims to plot different portions of the packet delay data
    parser.add_argument('--src-dest-relationship',
                        help='Process only packets of related relationship',
                        choices=[rel.name for rel in SrcDestRelationship], default='Any')
    parser.add_argument('--nodes-per-group', type=int, help='Assuming a 1-D dragonfly network, '
                        'this indicates the number of nodes per group (only useful with '
                        '--src-dest-relationship)', default=8)
    parser.add_argument('--nodes-per-router', type=int, help='Assuming a 1-D dragonfly network, '
                        'this indicates the number of nodes per router (only useful with '
                        '--src-dest-relationship)', default=2)
    parser.add_argument('--use-cython', type=bool, help='Total (virtual) simulation time',
                        default=False)
    args = parser.parse_args()

    plotting = False
    computing = True

    loading = not computing
    end_time = args.end
    n_windows = args.windows

    dist_type = getattr(SrcDestRelationship, args.src_dest_relationship)

    out_file_name = f"{args.output}.npz"

    if computing:
        if args.use_cython:
            assert dist_type == SrcDestRelationship.Any
            import pyximport; pyximport.install(language_level='3str')  # noqa: E702
            from file_read_cython.read_mean_std_from_file import load_mean_and_std_through_window

            windows, n_samples, samples = load_mean_and_std_through_window(
                str(args.latencies), args.start, args.end, num_windows=args.windows,
                max_rows=100000)
            means, stds = samples[:, 0], samples[:, 1]

        else:
            # Columns within the csv file that matter to us
            header, delays = collect_data_numpy(
                args.latencies, 'packets-delay', delimiter=',',
                dtype=np.dtype('float'))
            next_packet_delay_col = header.index('next_packet_delay')
            end_time_col = header.index('end')
            delay_col = header.index('latency')

            delays = delays[delays[:, next_packet_delay_col] > 0]
            delays = delays[delays[:, end_time_col] > 0]
            delays = break_delay_data_into(
                delays, dist_type,
                nodes_per_group=args.nodes_per_group, nodes_per_router=args.nodes_per_router)

            # Computing windowed mean and stds + plotting
            windows, means, stds, n_samples = find_mean_and_std_through_window(
                delays, n_windows=n_windows, end_time_col=end_time_col,
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
