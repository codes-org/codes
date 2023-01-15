from __future__ import annotations

import numpy as np
import glob
import fileinput
import sys
import pathlib

from typing import Any


def collect_data_numpy(
    path: pathlib.Path | str,
    filepreffix: str,
    delimiter: str | None = None,
    dtype: Any = int
) -> np.ndarray[Any, Any]:
    escaped_path = pathlib.Path(glob.escape(path))  # type: ignore
    stat_files = glob.glob(str(escaped_path / f"{filepreffix}-gid=*.txt"))
    if not stat_files:
        print(f"No valid `{filepreffix}` files have been found in path {path}", file=sys.stderr)
        exit(1)

    return np.loadtxt(fileinput.input(stat_files), delimiter=delimiter, dtype=dtype)


if __name__ == '__main__':
    delays = collect_data_numpy('.', 'packets-delay', delimiter=',',
                                dtype=np.dtype('float'))
    # sorting by source terminal and packet id
    sorted_indx = np.lexsort((delays[:, 2], delays[:, 0]))
    delays = delays[sorted_indx]

    # saving some columns
    np.savetxt("packets-delay.csv", delays[:, (0, 1, 2, 3, 5)],
               fmt="%d,%d,%d,%f,%f",
               header='src_terminal,dst_terminal,packet_id,start_time,delay',
               comments='')
