import argparse
import os
import random
import time
import warnings
from itertools import product
from pathlib import Path

import pandas as pd
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from sklearn.preprocessing import MinMaxScaler


warnings.filterwarnings("ignore")

class MLP(nn.Module):
    def __init__(self, total_terminals, max_packet_size, h_dim, out_dim, norm_max_list,
                 norm_min_list, channels=1):
        super().__init__()
        self.norm_max_list = norm_max_list
        self.norm_min_list = norm_min_list
        self.total_terminals = total_terminals
        self.max_packet_size = max_packet_size
        self.channels = channels
        self.weights = nn.Parameter(torch.Tensor(channels, total_terminals, total_terminals))
        self.reg = nn.Sequential(
                nn.Linear(channels + 2, h_dim),
                nn.ReLU(),
                nn.Linear(h_dim, out_dim),
            )

        nn.init.uniform_(self.weights, 0, 1)

    def forward(self, input_seq):
        tt = self.total_terminals
        input_src_terminal = F.one_hot(input_seq[:, 0], num_classes=tt)
        input_src_terminal = input_src_terminal.reshape((-1, 1, 1, tt)).float()
        input_dest_terminal = F.one_hot(input_seq[:, 1], num_classes=tt)
        input_dest_terminal = input_dest_terminal.reshape((-1, 1, tt, 1)).float()

        # assuming the input was a single row, it could be written as matrix
        # multiplication as
        # combined = input_src_terminal @ self.weights @ input_dest_terminal.T
        combined = torch.matmul(torch.matmul(input_src_terminal, self.weights), input_dest_terminal)

        input_size = input_seq[:, 2].reshape((-1, 1)).float() / self.max_packet_size
        input_is_there_another = input_seq[:, 3].reshape((-1, 1)).float()

        input_seq = torch.concat(
            (combined.reshape((-1, self.channels)),
             input_size,  # size
             input_is_there_another,  # is_there_another_pckt_in_queue
             ),
            dim=1
        ).float()

        pred = self.reg(input_seq)

        if not self.training:
            pred = self.denormalize(pred)
        return pred

    def denormalize(self, pred_norm):
        pred = torch.zeros(pred_norm.shape)
        for i in range(pred_norm.shape[1]):
            pred[:, i] = pred_norm[:, i]*(self.norm_max_list[i] - self.norm_min_list[i]) + self.norm_min_list[i]

        return pred

def split(data):
    # removing packets with no info and shuffle data
    noinfo_index = (data['next_packet_delay'] != -1)
    noinfo2_index = (data['is_there_another_pckt_in_queue'] != 0)
    data = data[np.bitwise_and(noinfo_index, noinfo2_index)]
    data = data.sample(frac=1, random_state=1)

    #split data
    train_data = data[0:int(0.8*len(data))]
    test_data = data[int(0.8*len(data)):]

    return train_data, test_data

def extract_process_data(train_data, X_columns, Y_columns):
    # encode input data with one-hot encoding
    # categories = np.unique(train_data[X_columns[0]].values)
    # X_train = np.zeros((train_data.shape[0], len(X_columns), len(categories)))
    # for i in range(len(X_columns)):
    #     column = train_data[X_columns[i]].values
    #     categories = np.unique(column)
    #     X_train[:, i, :] = np.array([np.array(item == categories, dtype=int) for item in column])
    X_train = train_data[X_columns].values

    # normalize output data with minimax
    scaler = MinMaxScaler()
    Y_train = scaler.fit_transform(train_data[Y_columns].values) # x_normalized = (x-x_min)/(x_max-x_min)
    norm_max_list, norm_min_list = scaler.data_max_, scaler.data_min_

    return X_train, Y_train, norm_max_list, norm_min_list

def extract_data(test_data, X_columns, Y_columns):
    X_test = test_data[X_columns].values
    Y_test = test_data[Y_columns].values

    return X_test, Y_test

def cal_rmse(pred, test):
    return np.sqrt(np.mean(np.square(pred - test)))


def main_func(args):

    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)
    if torch.cuda.is_available():
        torch.cuda.empty_cache()
        torch.cuda.manual_seed(args.seed)
        torch.cuda.manual_seed_all(args.seed)
    os.environ['PYTHONHASHSEED'] = str(args.seed)
    os.environ['OMP_NUM_THREADS'] = '1'
    torch.backends.cudnn.deterministic = True
    torch.backends.cudnn.benchmark = False

    data = pd.read_csv(args.input_file)
    # print(data)

    train_data, test_data = split(data)

    start_time = time.time()

    if args.method == 'MLP':
        X_columns = ['src_terminal', 'dest_terminal', 'size', 'is_there_another_pckt_in_queue']
        Y_columns = ['latency', 'next_packet_delay']
        X_train, Y_train, norm_max_list, norm_min_list = extract_process_data(train_data, X_columns, Y_columns)
        X_test, Y_test = extract_data(test_data, X_columns, Y_columns)
        y_dim = Y_train.shape[1]

        if args.load_model:
            print("Loading model from disk")
            mlp = torch.jit.load(args.model_path)
        else:
            print("Generating model from scratch")
            mlp = MLP(args.terminals, args.pck_size, args.h_dim, y_dim, torch.FloatTensor(norm_max_list), torch.FloatTensor(norm_min_list))

        optimizer = torch.optim.Adam(mlp.parameters(), lr=0.001)
        loss_function = nn.MSELoss()
        mlp.train()

        all_idx = list(range(len(X_train)))
        random.shuffle(all_idx)

        batch_size = 1024
        batch_num = len(X_train) // batch_size if len(X_train) % batch_size == 0 else len(X_train) // batch_size + 1

        for i in range(args.epoch):
            epoch_loss = 0
            for batch_idx in range(batch_num):
                is_final_batch = (batch_idx == (batch_num - 1))

                if not is_final_batch:
                    idx = all_idx[batch_idx * batch_size: (batch_idx + 1) * batch_size]
                else:
                    idx = all_idx[batch_idx * batch_size:]

                x, y = X_train[idx], Y_train[idx]

                x, y = torch.LongTensor(x), torch.FloatTensor(y)

                optimizer.zero_grad()
                y_pred = mlp(x)
                loss = loss_function(y_pred, y)
                loss.backward()
                optimizer.step()
                epoch_loss = epoch_loss + loss

            print(i, epoch_loss)

        # EVAL has to be called before saving the state of the network
        mlp.eval()

        mlp_scripted = torch.jit.script(mlp)
        mlp_scripted.save(args.model_path)

        X_test = torch.LongTensor(X_test)
        with torch.no_grad():
            Y_pred = mlp(X_test).numpy()

        rmse = cal_rmse(Y_pred, Y_test)

    elif args.method == 'Average':
        train_data = train_data[['src_terminal', 'dest_terminal', 'latency']]
        test_data = test_data[['src_terminal', 'dest_terminal', 'latency']]

        mean_src_dest = train_data.groupby(['src_terminal', 'dest_terminal']).mean()
        mean_src = train_data.groupby(['src_terminal']).mean()
        mean_dest = train_data.groupby(['dest_terminal']).mean()
        total_avg = train_data.values.mean()

        terminal2terminal = np.zeros((72, 72))
        # terminal2terminal = np.ones((72, 72)) * total_avg
        for i, j in product(range(72), range(72)):
            if mean_src_dest.index.isin([(i, j)]).any():
                latency = mean_src_dest.loc[(i, j), 'latency']
            elif mean_src.index.isin([i]).any() == True and  mean_dest.index.isin([j]).any()== False:
                latency = mean_src.loc[i, 'latency'].item()
            elif mean_src.index.isin([i]).any() == False and  mean_dest.index.isin([j]).any()== True:
                latency = mean_dest.loc[j, 'latency'].item()
            else:
                latency = total_avg
            terminal2terminal[i, j] = latency

        items = test_data[['src_terminal', 'dest_terminal']].values
        src = items[:, 0]
        dest = items[:, 1]
        pred = terminal2terminal[src, dest]

        rmse = cal_rmse(pred, test_data['latency'].values)

    end_time = time.time()

    print('rmse:', rmse)
    print('Time:', end_time - start_time)

    if args.plot_weights:
        if args.method == 'MLP':
            with torch.no_grad():
                terminal2terminal = mlp.weights.numpy()
                terminal2terminal = terminal2terminal[0, :, :]  # extracting first channel weights

        import matplotlib.pyplot as plt
        fig, ax = plt.subplots()
        c = ax.imshow(terminal2terminal, cmap='RdBu', interpolation='nearest')
        fig.colorbar(c, ax=ax)
        plt.show()

if __name__ == '__main__':
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

    args = parser.parse_args()

    main_func(args)
