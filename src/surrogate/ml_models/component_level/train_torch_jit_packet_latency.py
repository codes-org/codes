#!/usr/bin/env python3
"""Train/export a TorchScript terminal/default packet-latency model.

Input CSV rows come from PARAMS.save_packet_latency_path. The model contract is:
  input  FloatTensor[*,12]
  output FloatTensor[*,2] = [travel_delta, next_packet_delay]
"""
import argparse
from pathlib import Path
import glob

import numpy as np
import pandas as pd
import torch

def resolve_device(name: str) -> torch.device:
    if name == "auto":
        name = "cuda" if torch.cuda.is_available() else "cpu"
    if name == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA was requested, but torch.cuda.is_available() is false.")
    device = torch.device(name)
    print(f"Using device: {device}")
    if device.type == "cuda":
        print(f"CUDA device: {torch.cuda.get_device_name(0)}")
    return device

from torch import nn
from torch.utils.data import DataLoader, TensorDataset

FEATURE_COLUMNS = [
    "src_terminal", "dest_terminal", "packet_size", "is_there_another_pckt_in_queue",
    "caller_lp_gid", "src_router_id", "src_group_id", "dst_router_id", "dst_group_id",
    "terminal_queue_length", "terminal_vc_occupancy", "processing_packet_delay",
]
TARGET_COLUMNS = ["travel_end_time_delta", "next_packet_delay"]
ALL_COLUMNS = FEATURE_COLUMNS + TARGET_COLUMNS + [
    "packet_id", "is_surrogate_on", "is_predicted", "workload_injection", "start", "end", "latency"
]

class MLP(nn.Module):
    def __init__(self, in_dim: int, out_dim: int, hidden: int):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(in_dim, hidden), nn.ReLU(),
            nn.Linear(hidden, hidden), nn.ReLU(),
            nn.Linear(hidden, out_dim),
        )
    def forward(self, x):
        return self.net(x.float())

def read_trace(path: str) -> pd.DataFrame:
    p = Path(path)
    files = sorted(glob.glob(str(p / "packets-delay-gid=*.txt"))) if p.is_dir() else [str(p)]
    if not files:
        raise SystemExit(f"No packet latency trace files found under {path}")
    frames = [pd.read_csv(f, comment="#", names=ALL_COLUMNS) for f in files]
    df = pd.concat(frames, ignore_index=True)
    df = df[(df["is_surrogate_on"] == 0) & (df["is_predicted"] == 0)]
    df = df.replace([np.inf, -np.inf], np.nan).dropna(subset=FEATURE_COLUMNS + TARGET_COLUMNS)
    df = df[(df["travel_end_time_delta"] > 0.0) & (df["next_packet_delay"] >= 0.0)]
    if df.empty:
        raise SystemExit("No usable high-fidelity packet latency rows after filtering")
    return df

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True, help="Trace file or directory from save_packet_latency_path")
    ap.add_argument("--out", required=True, help="Output TorchScript .pt path")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--batch-size", type=int, default=1024)
    ap.add_argument("--hidden", type=int, default=128)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda"],
                    help="Training device. auto uses CUDA if available, otherwise CPU.")
    args = ap.parse_args()
    device = resolve_device(args.device)

    df = read_trace(args.trace)
    x = torch.tensor(df[FEATURE_COLUMNS].to_numpy(dtype=np.float32), device=device)
    y = torch.tensor(df[TARGET_COLUMNS].to_numpy(dtype=np.float32), device=device)

    x_mu, x_sigma = x.mean(0), x.std(0).clamp_min(1e-6)
    y_mu, y_sigma = y.mean(0), y.std(0).clamp_min(1e-6)
    x_n = (x - x_mu) / x_sigma
    y_n = (y - y_mu) / y_sigma

    model = MLP(len(FEATURE_COLUMNS), len(TARGET_COLUMNS), args.hidden).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr)
    loss_fn = nn.SmoothL1Loss()
    loader = DataLoader(TensorDataset(x_n, y_n), batch_size=args.batch_size, shuffle=True)

    model.train()
    for epoch in range(args.epochs):
        losses = []
        for xb, yb in loader:
            opt.zero_grad(set_to_none=True)
            loss = loss_fn(model(xb), yb)
            loss.backward()
            opt.step()
            losses.append(float(loss.detach()))
        print(f"epoch={epoch+1} loss={np.mean(losses):.6g}")

    class NormalizedWrapper(nn.Module):
        def __init__(self, base, x_mu, x_sigma, y_mu, y_sigma):
            super().__init__()
            self.base = base.eval()
            self.register_buffer("x_mu", x_mu)
            self.register_buffer("x_sigma", x_sigma)
            self.register_buffer("y_mu", y_mu)
            self.register_buffer("y_sigma", y_sigma)
        def forward(self, x):
            y = self.base((x.float() - self.x_mu) / self.x_sigma)
            return y * self.y_sigma + self.y_mu

    # Export a CPU TorchScript module. Training may happen on CUDA, but the
    # C++ surrogate path should be able to load and run this model on CPU.
    model = model.to("cpu").eval()
    x_mu = x_mu.detach().to("cpu")
    x_sigma = x_sigma.detach().to("cpu")
    y_mu = y_mu.detach().to("cpu")
    y_sigma = y_sigma.detach().to("cpu")

    wrapped = NormalizedWrapper(model, x_mu, x_sigma, y_mu, y_sigma).to("cpu").eval()
    example = torch.zeros(1, len(FEATURE_COLUMNS), dtype=torch.float32)

    with torch.no_grad():
        traced = torch.jit.trace(wrapped, example)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    traced.save(args.out)
    print(f"saved {args.out}")

if __name__ == "__main__":
    main()
