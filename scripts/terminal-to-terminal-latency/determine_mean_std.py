import numpy as np
import matplotlib.pyplot as plt


def mean_and_std(array: np.array) -> tuple[float, float]:
    return np.mean(array), np.std(array)  # type: ignore


if __name__ == '__main__':
    delays = np.loadtxt("packets-delay.csv", skiprows=1, delimiter=",")
    start_col = 8
    delay_col = 9

    # Filtering data to some interval
    delays = delays[np.bitwise_and(delays[:, start_col] > 200e3,
                                   delays[:, start_col] + delays[:, delay_col] < 500e3)]

    # Distribution
    delays_same_router = (delays[:, 0] // 2) == (delays[:, 1] // 2)
    delays_same_group = np.bitwise_xor(
        (delays[:, 0] // 8) == (delays[:, 1] // 8),
        delays_same_router)
    delays_out_group = (delays[:, 0] // 8) != (delays[:, 1] // 8)

    mean, std = mean_and_std(delays[:, delay_col])
    print(f"total mean: {mean:.2f} std: {std:.2f}")
    print()

    delays0 = delays[delays[:, 0] == 0]
    mean, std = mean_and_std(delays0[:, delay_col])
    print(f"terminal 0 mean: {mean:.2f} std: {std:.2f}")
    print()

    fig, axs = plt.subplots(2, 2)
    axs[0, 0].set_title("Latency from all terminals to all")
    # axs[0, 0].set_xlabel("latency")
    axs[0, 0].hist(delays[:, delay_col], bins=50, density=True, alpha=0.6, color='b')
    axs[0, 1].set_title("Latency to terminals in same router")
    # axs[0, 1].set_xlabel("latency")
    axs[0, 1].hist(delays[delays_same_router, delay_col], bins=50, density=True, alpha=0.6, color='b')
    axs[1, 0].set_title("Latency to terminals in same group")
    axs[1, 0].set_xlabel("latency")
    axs[1, 0].hist(delays[delays_same_group, delay_col], bins=50, density=True, alpha=0.6, color='b')
    axs[1, 1].set_title("Latency to terminals in other groups")
    axs[1, 1].set_xlabel("latency")
    axs[1, 1].hist(delays[delays_out_group, delay_col], bins=50, density=True, alpha=0.6, color='b')
    plt.show()

    buckets = [delays0[delays0[:, 1] == i] for i in range(1, 72)]
    buckets_processed = np.array([mean_and_std(b[:, delay_col]) for b in buckets])
    print("Destination, Means and stds for terminal 0")
    for i, (mean, std) in enumerate(buckets_processed):
        print(f"{i+1}, {mean:.2f}, {std:.2f}")
    print()

    mean, std = mean_and_std(delays[delays_same_router, delay_col])
    print(f"same router mean: {mean:.2f} std: {std:.2f}")
    print()

    mean, std = mean_and_std(delays[delays_same_group, delay_col])
    print(f"same group mean: {mean:.2f} std: {std:.2f} (excluding same router)")
    print()

    mean, std = mean_and_std(delays[delays_out_group, delay_col])
    print(f"other groups mean: {mean:.2f} std: {std:.2f}")
