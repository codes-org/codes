import numpy as np
import matplotlib.pyplot as plt


if __name__ == '__main__':
    port_utilization = np.loadtxt("dragonfly-snapshots.csv", delimiter=',', dtype=float, skiprows=1)

    # finding all snapshot timestamps
    timestamps = np.unique(port_utilization[:, 0])
    assert len(timestamps.shape) == 1

    # Finding total utilization per snapshot
    total_utilization = np.zeros_like(timestamps)
    for i, ts in enumerate(timestamps):
        total_utilization[i] = port_utilization[port_utilization[:, 0] == ts, 2:].sum()

    # plotting
    plt.plot(timestamps, total_utilization)
    plt.xlabel('snapshot time (ns)')
    plt.ylabel('total buffer port occupancy')
    plt.show()
