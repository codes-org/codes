from pathlib import Path
import glob
import fileinput
import numpy as np

from libc.math cimport floor, sqrt
cimport cython


@cython.boundscheck(False)  # turn off bounds-checking for entire function
@cython.wraparound(False)   # turn off wrapping (negative numbers) for entire function
def load_mean_and_std_through_window(
    str filepath,
    double start_time,
    double end_time,
    int num_windows = 100,
    int max_rows = 10000
):
    cdef int num_lines
    cdef int i
    cdef int window_j
    cdef double[:] windows
    cdef double[:, :] packet_latency_data
    cdef double[:, :] samples
    cdef int[:] n_samples
    cdef double window_time = (end_time - start_time) / num_windows

    samples = np.zeros((num_windows, 2), dtype=np.double)
    n_samples = np.zeros((num_windows,), dtype=np.int32)
    windows = np.zeros((num_windows,), dtype=np.double)

    stat_files = glob.glob(str(Path(filepath) / "packets-delay-gid=*.txt"))

    with open(stat_files[0], 'r') as f:
        header = f.readline()[1:].split(',')
    cdef int end_time_col = header.index('end')
    cdef int delay_col = header.index('latency')

    # Finding mean of data
    raw_files = fileinput.input(stat_files, mode='rb')
    while True:
        data_raw = np.loadtxt(
            raw_files, delimiter=',', dtype=np.double,
            comments='#', max_rows=max_rows)
        if data_raw.size == 0 or len(data_raw.shape) != 2:
            break

        packet_latency_data = data_raw
        num_lines = packet_latency_data.shape[0]

        assert(num_lines != 0)

        for i in range(num_lines):
            window_j = int(floor((packet_latency_data[i, end_time_col] - start_time) / window_time))
            if window_j < 0 or window_j >= num_windows:
                continue
            samples[window_j, 0] += packet_latency_data[i, delay_col]
            n_samples[window_j] += 1
    raw_files.close()

    for i in range(num_windows):
        # Computing mean
        if n_samples[i] > 0:
            samples[i, 0] /= n_samples[i]
        windows[i] = (i+1) * window_time

    # Finding mean of data
    raw_files = fileinput.input(stat_files, mode='rb')
    while True:
        data_raw = np.loadtxt(
            raw_files, delimiter=',', dtype=np.double,
            comments='#', max_rows=10000)
        if data_raw.size == 0 or len(data_raw.shape) != 2:
            break

        packet_latency_data = data_raw
        num_lines = packet_latency_data.shape[0]

        assert(num_lines != 0)

        for i in range(num_lines):
            window_j = int(floor((packet_latency_data[i, end_time_col] - start_time) / window_time))
            if window_j < 0 or window_j >= num_windows:
                continue
            samples[window_j, 1] += (packet_latency_data[i, delay_col] - samples[window_j, 0]) ** 2
    raw_files.close()

    for i in range(num_windows):
        # Computing std
        if n_samples[i] > 0:
            samples[i, 1] = sqrt(samples[i, 1] / n_samples[i])

    return np.asarray(windows), np.asarray(n_samples), np.asarray(samples)
