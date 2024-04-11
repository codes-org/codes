# Adapted from example from matplotlib lib

from typing import Any, TextIO
import argparse
import pathlib

import matplotlib.pyplot as plt
import matplotlib
import numpy as np


def plot_sequence(ax: Any, seq: Any, names: Any, height: Any, color: str = 'red', print_names: bool = True):
    ax.vlines(seq, 0, height, color=f"tab:{color}")  # The vertical stems.
    ax.plot(seq, np.zeros_like(seq), "-o", color="k", markerfacecolor="w")
    
    # annotate lines
    if print_names:
        for d, h, r in zip(seq, height, names):
            ax.annotate(r, xy=(d, h),
                        xytext=(3, np.sign(h)*3), textcoords="offset points",
                        horizontalalignment="right",
                        verticalalignment="bottom" if h > 0 else "top")


# hardcoded data
def iterations_count_example():
    iterations = np.array([5700202, 11141148, 16735521, 22248304, 28018657, 33344653, 39131394, 44535575, 49924184, 55265978, 60797003, 65999354, 71477966, 77089252, 82388323, 87510575, 92672984, 97968684, 103413575, 108791049, 114191370, 119281369, 124947369, 130269516, 135814413, 140706572, 146191543, 152244928, 157549505, 163252774])
    names = np.arange(iterations.size)
    # height = np.ones_like(names)
    height = iterations.astype(np.float64)
    height[1:] -= iterations[:-1]
    # mean_height = height.mean()
    # height /= mean_height
    
    iterations2 = np.array([4475938, 8527507, 12500772, 16932824, 21122232, 24629352, 28727112, 32812390, 37119760, 40873748, 44831210, 49236742, 53495581, 57186915, 61102874, 65089296, 69034116, 72827668, 77306215, 81505333, 84962239, 88817963, 92788913, 97258245, 101298185, 105234798, 109230081, 113176951, 117033360, 120922482, 125158680, 129445759, 132927795, 136967719, 140707240, 144980904, 148570317, 152949619, 157429076, 161858572, 165599534, 169169124, 172576205, 176267989, 179822127, 183531146, 187147511, 190685445, 194270774, 197863388, 201349592, 204959427, 208557228, 212286717, 215720477, 219201662, 222629090, 226452092, 230156036, 233856397, 237545455, 241265332, 245016561, 248662995, 252212229, 255620388, 259105490, 262543988, 266118703, 269713894, 273230378, 276923706, 280425248, 284046990, 287508037, 291266834, 294812966, 298512239, 302113836, 305636975, 309307151, 312842662, 316463094, 320055020, 323542940, 327139573, 330811189, 334388299, 337788549, 341498322, 345104703, 348880050, 352448690, 356106442, 359506153, 363094952, 366703208, 370233755, 373770752, 377222496])
    names2 = np.arange(iterations2.size)
    # height2 = -1 * np.ones_like(names2)
    height2 = iterations2.astype(np.float64)
    height2[1:] -= iterations2[:-1]
    # height2 /= mean_height
    height2 *= -1

    return (iterations, names, height), (iterations2, names2, height2)


# class JobAvgIterations(TypedDict):
#     iterations: 


# typing cannot be done for structured arrays :S
def parse_iteration_log(log_file: TextIO):
    log_pattern = r'ITERATION (\d+) node \d+ job (\d+) rank \d+ time (\d*\.?\d+)\n'
    log_iters = np.fromregex(log_file, log_pattern, [('iter', np.int64), ('job', np.int64), ('time', np.float64)])

    def get_avg_for_iters(job: np.int64):
        def avg(it: np.int64) -> np.float64:
            matched_iters = log_iters[np.bitwise_and(log_iters['job'] == job, log_iters['iter'] == it)]
            return np.mean(matched_iters['time'].astype(np.float64))
        return avg

    jobs: dict[int, np.ndarray[Any, Any]] = {}
    for job in np.unique(log_iters['job']):
        iterations = np.unique(log_iters[log_iters['job'] == job]['iter'])
        # avg_timestamp = np.vectorize(get_avg_for_iters(job), otypes=(np.float64,))(iterations)
        avg_timestamp = np.array([get_avg_for_iters(job)(it) for it in iterations])
        assert(iterations.size == avg_timestamp.size)

        # finding time that each iteration took
        avg_iter_time = avg_timestamp.copy()
        avg_iter_time[1:] -= avg_timestamp[:-1]
        # "removing" iterations for which we don't know how much they actually took
        to_rem = iterations.copy()
        to_rem[1:] -= to_rem[:-1] + 1
        to_rem[0] = 0  # Assuming the first value hasn't been skipped
        avg_iter_time[to_rem != 0] = 0

        combined = np.zeros_like(iterations, dtype=[('iter', np.int64), ('time', np.float64), ('iter_time', np.float64)])
        combined['iter'] = iterations
        combined['time'] = avg_timestamp
        combined['iter_time'] = avg_iter_time
        jobs[int(job)] = combined

    return jobs


# if __name__ == "__main__":
#     (iterations, names, height), (iterations2, names2, height2) = iterations_count_example()
#     fig, ax = plt.subplots(figsize=(8.8, 4), layout="constrained")
#     plot_sequence(ax, iterations, names, height, 'blue')
#     plot_sequence(ax, iterations2, names2, height2, 'red')
#     plt.setp(ax.get_xticklabels(), rotation=30, ha="right")
#     plt.show()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    _ = parser.add_argument('file', type=argparse.FileType('r'))
    _ = parser.add_argument('--output', type=pathlib.Path, help='Name of output figure', default=None)
    _ = parser.add_argument('--no-iter-count', dest='iter_count', action='store_false')
    args = parser.parse_args()

    if args.output:
        matplotlib.use("pgf")
        matplotlib.rcParams.update({
            "pgf.texsystem": "pdflatex",
            'font.family': 'serif',
            'font.size': 16,
            'text.usetex': True,
            'pgf.rcfonts': False,
        })

    parsed_logs = parse_iteration_log(args.file)

    # Creating plot with data
    fig, ax = plt.subplots(figsize=(8.8, 4), layout="constrained")
    ax.set_xlabel("Total virtual time (ns)")
    ax.set_ylabel("Virtual time per iteration (ns)")
    #ax.set(title="")
    smallest_timestamp = list(parsed_logs.values())[0]['time'][0]
    ax.plot([0, smallest_timestamp], [0, 0], "-", color="k", markerfacecolor="w")

    color_table = ['red', 'blue', 'green', 'black']
    for i, job in enumerate(parsed_logs.keys()):
        # Flipping second sequence if there are only two jobs
        mul = -1 if len(parsed_logs) == 2 and i == 1 else 1
        plot_sequence(
            ax,
            parsed_logs[job]['time'],
            parsed_logs[job]['iter'],
            mul * parsed_logs[job]['iter_time'],
            color=color_table[i],
            print_names=args.iter_count)
    
    plt.setp(ax.get_xticklabels(), rotation=30, ha="right")
    
    #ax.margins(y=0.1)
    if args.output:
        plt.tight_layout()
        plt.savefig(f'{args.output}.pgf', bbox_inches='tight')
        plt.savefig(f'{args.output}.pdf', bbox_inches='tight')
    else:
        plt.show()
