#!/usr/bin/env python3
"""Train/export a TorchScript router queueing-delay model.

Input CSV rows come from PARAMS.save_router_timing_trace_path. The model contract is:
  input  FloatTensor[*,12]
  output FloatTensor[*,1] = [router_queueing_delay]
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
    "router_id", "group_id", "output_port", "output_chan", "to_terminal", "is_global",
    "packet_size", "chunk_size", "output_vc_occupancy", "output_queued_count",
    "next_output_available_delta", "nominal_router_delay",
]
TARGET_COLUMNS = ["router_queueing_delay"]
ALL_COLUMNS = FEATURE_COLUMNS + TARGET_COLUMNS + [
    "actual_router_delay", "is_surrogate_on", "is_predicted", "now", "packet_id",
    "src_terminal", "dest_terminal", "next_stop"
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

def _filter_router_rows(df: pd.DataFrame) -> pd.DataFrame:
    df = df[(df["is_surrogate_on"] == 0) & (df["is_predicted"] == 0)]
    df = df.replace([np.inf, -np.inf], np.nan).dropna(subset=FEATURE_COLUMNS + TARGET_COLUMNS)
    df = df[df["router_queueing_delay"] >= 0.0]
    return df


def read_trace(path: str, max_rows: int, seed: int, chunksize: int) -> pd.DataFrame:
    """Read router timing traces without materializing the full trace.

    If max_rows > 0, maintain a bounded random sample while streaming chunks.
    This still trains on real high-fidelity rows, but avoids loading a 40+ GB
    CSV trace into memory at once.
    """
    p = Path(path)
    files = sorted(glob.glob(str(p / "router-timing-gid=*.txt"))) if p.is_dir() else [str(p)]
    if not files:
        raise SystemExit(f"No router timing trace files found under {path}")

    rng = np.random.default_rng(seed)
    kept = []
    total_seen = 0
    total_usable = 0

    for f in files:
        print(f"reading {f}", flush=True)
        reader = pd.read_csv(
            f,
            comment="#",
            names=ALL_COLUMNS,
            chunksize=chunksize,
        )

        for chunk in reader:
            total_seen += len(chunk)
            chunk = _filter_router_rows(chunk)
            if chunk.empty:
                continue

            total_usable += len(chunk)

            if max_rows <= 0:
                kept.append(chunk)
                continue

            kept.append(chunk)
            df = pd.concat(kept, ignore_index=True)

            if len(df) > max_rows:
                # Randomly keep max_rows rows from all usable rows seen so far.
                # Use a fresh integer seed from rng so repeated chunks do not
                # produce identical samples.
                sample_seed = int(rng.integers(0, 2**31 - 1))
                df = df.sample(n=max_rows, random_state=sample_seed).reset_index(drop=True)

            kept = [df]

    if not kept:
        raise SystemExit("No usable high-fidelity router timing rows after filtering")

    df = pd.concat(kept, ignore_index=True)

    if max_rows > 0 and len(df) > max_rows:
        df = df.sample(n=max_rows, random_state=seed).reset_index(drop=True)

    print(f"router trace rows seen: {total_seen}", flush=True)
    print(f"usable high-fidelity rows seen: {total_usable}", flush=True)
    print(f"training rows kept: {len(df)}", flush=True)

    if df.empty:
        raise SystemExit("No usable high-fidelity router timing rows after filtering")

    return df

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True, help="Trace file or directory from save_router_timing_trace_path")
    ap.add_argument("--out", required=True, help="Output TorchScript .pt path")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--batch-size", type=int, default=1024)
    ap.add_argument("--hidden", type=int, default=128)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda"],
                    help="Training device. auto uses CUDA if available, otherwise CPU.")
    ap.add_argument("--max-rows", type=int, default=2000000,
                    help="Maximum usable router rows to keep. Use <=0 to load all rows.")
    ap.add_argument("--read-chunksize", type=int, default=1000000,
                    help="CSV rows per pandas chunk while streaming router traces.")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    device = resolve_device(args.device)

    df = read_trace(
        args.trace,
        max_rows=args.max_rows,
        seed=args.seed,
        chunksize=args.read_chunksize,
    )
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
