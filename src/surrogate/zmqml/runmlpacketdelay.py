
import argparse
from pathlib import Path

from model import mlpacketdelay

def run_training(done_event):
    parser = argparse.ArgumentParser(description="Delay Prediction")
    parser.add_argument('--method', type=str, default='MLP', choices=['MLP','Average'])
    parser.add_argument('--epoch', type=int, default=10, help='epochs to train')
    parser.add_argument('--h-dim', type=int, default=16, help='dimension of the hidden layer')
    parser.add_argument('--seed', type=int, default=0)
    parser.add_argument('--pck_size', type=int, default=4096, help='maximum packet size in simulation')
    parser.add_argument('--terminals', type=int, default=72, help='total number of terminals in the network')
    parser.add_argument('--input-file', type=Path, default=Path('packet-delays.txt'))
#    parser.add_argument('--load-model', action=argparse.BooleanOptionalAction, default=False,
    parser.add_argument('--load-model', action='store_true', default=False,
                        help='whether to load model from file or start from scratch')
    parser.add_argument('--model-path', type=Path, default=Path('MLP_Surrogate-combined.pt'))
#    parser.add_argument('--plot-weights', action=argparse.BooleanOptionalAction, default=False,
    parser.add_argument('--plot-weights', action='store_true', default=False,
                        help='whether to show weights from source to destination')

    args = parser.parse_args(["--method", "MLP", "--epoch", "1", # 50
                              "--input-file", "model/data/packets-delay.csv",
                              "--model-path", "ml-model.pt"])

    mlpacketdelay.main_func(args)
    done_event.set()
