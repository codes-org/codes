#!/usr/bin/env python3
"""
Train an LP-aware packet-latency TorchScript model from CODES-generated CSV data.

Expected input CSV:
    ml_training_data/lp_aware_packet_latency_training.csv

Input feature order must match the C++ Torch-JIT LP-aware mode:
    0  src_terminal
    1  dest_terminal
    2  packet_size
    3  is_there_another_pckt_in_queue
    4  caller_lp_gid
    5  src_router_id
    6  src_group_id
    7  dst_router_id
    8  dst_group_id
    9  terminal_queue_length
    10 terminal_vc_occupancy
    11 processing_packet_delay

Targets:
    0  travel_end_time_delta
    1  next_packet_delay
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import pandas as pd
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, TensorDataset, random_split


FEATURE_COLUMNS = [
    "src_terminal",
    "dest_terminal",
    "packet_size",
    "is_there_another_pckt_in_queue",
    "caller_lp_gid",
    "src_router_id",
    "src_group_id",
    "dst_router_id",
    "dst_group_id",
    "terminal_queue_length",
    "terminal_vc_occupancy",
    "processing_packet_delay",
]

TARGET_COLUMNS = [
    "travel_end_time_delta",
    "next_packet_delay",
]


class Standardizer:
    def __init__(self) -> None:
        self.mean: np.ndarray | None = None
        self.std: np.ndarray | None = None

    def fit(self, x: np.ndarray) -> "Standardizer":
        self.mean = x.mean(axis=0)
        self.std = x.std(axis=0)
        self.std[self.std < 1e-8] = 1.0
        return self

    def transform(self, x: np.ndarray) -> np.ndarray:
        if self.mean is None or self.std is None:
            raise RuntimeError("Standardizer must be fit before transform.")
        return (x - self.mean) / self.std

    def inverse_transform(self, x: np.ndarray) -> np.ndarray:
        if self.mean is None or self.std is None:
            raise RuntimeError("Standardizer must be fit before inverse_transform.")
        return x * self.std + self.mean

    def to_dict(self) -> dict:
        if self.mean is None or self.std is None:
            raise RuntimeError("Standardizer must be fit before serialization.")
        return {
            "mean": self.mean.tolist(),
            "std": self.std.tolist(),
        }


class LPAwarePacketLatencyModel(nn.Module):
    def __init__(
        self,
        input_dim: int = len(FEATURE_COLUMNS),
        output_dim: int = len(TARGET_COLUMNS),
        hidden_dim: int = 128,
        dropout: float = 0.0,
    ) -> None:
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(input_dim, hidden_dim),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(hidden_dim, output_dim),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = x.to(torch.float32)
        return self.net(x)


class TorchScriptInferenceWrapper(nn.Module):
    """
    Wrapper exported to C++.

    C++ passes raw 12-feature float32 vectors. This wrapper normalizes inputs,
    runs the trained model, de-normalizes outputs, and clamps outputs to be
    non-negative.
    """

    def __init__(
        self,
        model: nn.Module,
        x_mean: torch.Tensor,
        x_std: torch.Tensor,
        y_mean: torch.Tensor,
        y_std: torch.Tensor,
    ) -> None:
        super().__init__()
        self.model = model
        self.register_buffer("x_mean", x_mean)
        self.register_buffer("x_std", x_std)
        self.register_buffer("y_mean", y_mean)
        self.register_buffer("y_std", y_std)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = x.to(torch.float32)
        x_norm = (x - self.x_mean) / self.x_std
        y_norm = self.model(x_norm)
        y = y_norm * self.y_std + self.y_mean
        return torch.clamp(y, min=0.0)


def load_training_data(csv_path: Path) -> tuple[np.ndarray, np.ndarray]:
    if not csv_path.exists():
        raise FileNotFoundError(f"Training CSV does not exist: {csv_path}")

    df = pd.read_csv(csv_path)

    missing = [c for c in FEATURE_COLUMNS + TARGET_COLUMNS if c not in df.columns]
    if missing:
        raise ValueError(f"CSV is missing required columns: {missing}")

    df = df.replace([np.inf, -np.inf], np.nan)
    before = len(df)
    df = df.dropna(subset=FEATURE_COLUMNS + TARGET_COLUMNS)
    after = len(df)

    if after == 0:
        raise ValueError("No usable rows after dropping NaN/inf values.")

    # Keep only physically meaningful targets.
    df = df[df["travel_end_time_delta"] > 0]
    df = df[df["next_packet_delay"] > 0]

    if len(df) == 0:
        raise ValueError("No rows with positive target values.")

    if len(df) < before:
        print(f"Dropped {before - len(df)} unusable rows.")

    x = df[FEATURE_COLUMNS].to_numpy(dtype=np.float32)
    y = df[TARGET_COLUMNS].to_numpy(dtype=np.float32)

    return x, y


def train(args: argparse.Namespace) -> None:
    torch.manual_seed(args.seed)
    np.random.seed(args.seed)

    x_raw, y_raw = load_training_data(args.csv)

    x_scaler = Standardizer().fit(x_raw)
    y_scaler = Standardizer().fit(y_raw)

    x = x_scaler.transform(x_raw).astype(np.float32)
    y = y_scaler.transform(y_raw).astype(np.float32)

    x_tensor = torch.from_numpy(x)
    y_tensor = torch.from_numpy(y)

    dataset = TensorDataset(x_tensor, y_tensor)

    val_size = max(1, int(len(dataset) * args.val_fraction))
    train_size = len(dataset) - val_size
    if train_size <= 0:
        raise ValueError(f"Not enough rows for train/val split: {len(dataset)}")

    train_ds, val_ds = random_split(
        dataset,
        [train_size, val_size],
        generator=torch.Generator().manual_seed(args.seed),
    )

    train_loader = DataLoader(
        train_ds,
        batch_size=args.batch_size,
        shuffle=True,
        drop_last=False,
    )
    val_loader = DataLoader(
        val_ds,
        batch_size=args.batch_size,
        shuffle=False,
        drop_last=False,
    )

    device = torch.device(args.device)
    model = LPAwarePacketLatencyModel(
        hidden_dim=args.hidden_dim,
        dropout=args.dropout,
    ).to(device)

    optimizer = torch.optim.AdamW(
        model.parameters(),
        lr=args.lr,
        weight_decay=args.weight_decay,
    )
    loss_fn = nn.MSELoss()

    best_val = float("inf")
    best_state: dict[str, torch.Tensor] | None = None

    for epoch in range(1, args.epochs + 1):
        model.train()
        train_losses = []

        for xb, yb in train_loader:
            xb = xb.to(device)
            yb = yb.to(device)

            optimizer.zero_grad(set_to_none=True)
            pred = model(xb)
            loss = loss_fn(pred, yb)
            loss.backward()

            if args.grad_clip > 0:
                torch.nn.utils.clip_grad_norm_(model.parameters(), args.grad_clip)

            optimizer.step()
            train_losses.append(loss.item())

        model.eval()
        val_losses = []
        with torch.no_grad():
            for xb, yb in val_loader:
                xb = xb.to(device)
                yb = yb.to(device)
                pred = model(xb)
                val_losses.append(loss_fn(pred, yb).item())

        train_loss = float(np.mean(train_losses))
        val_loss = float(np.mean(val_losses))

        if val_loss < best_val:
            best_val = val_loss
            best_state = {
                k: v.detach().cpu().clone()
                for k, v in model.state_dict().items()
            }

        if epoch == 1 or epoch % args.log_every == 0 or epoch == args.epochs:
            print(
                f"epoch={epoch:04d} "
                f"train_mse={train_loss:.6g} "
                f"val_mse={val_loss:.6g} "
                f"best_val_mse={best_val:.6g}"
            )

    if best_state is None:
        raise RuntimeError("Training failed to produce a best model state.")

    model.load_state_dict(best_state)
    model.eval()

    wrapper = TorchScriptInferenceWrapper(
        model.cpu(),
        x_mean=torch.tensor(x_scaler.mean, dtype=torch.float32).view(1, -1),
        x_std=torch.tensor(x_scaler.std, dtype=torch.float32).view(1, -1),
        y_mean=torch.tensor(y_scaler.mean, dtype=torch.float32).view(1, -1),
        y_std=torch.tensor(y_scaler.std, dtype=torch.float32).view(1, -1),
    )
    wrapper.eval()

    example = torch.zeros((1, len(FEATURE_COLUMNS)), dtype=torch.float32)

    with torch.no_grad():
        traced = torch.jit.trace(wrapper, example)
        test_out = traced(example)

    if tuple(test_out.shape) != (1, len(TARGET_COLUMNS)):
        raise RuntimeError(f"Unexpected traced output shape: {tuple(test_out.shape)}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    traced.save(str(args.output))

    metadata = {
        "feature_columns": FEATURE_COLUMNS,
        "target_columns": TARGET_COLUMNS,
        "input_shape": [None, len(FEATURE_COLUMNS)],
        "output_shape": [None, len(TARGET_COLUMNS)],
        "input_dtype": "float32",
        "output_dtype": "float32",
        "x_scaler": x_scaler.to_dict(),
        "y_scaler": y_scaler.to_dict(),
        "rows": int(len(x_raw)),
        "train_rows": int(train_size),
        "val_rows": int(val_size),
        "best_val_mse_scaled": best_val,
    }

    metadata_path = args.output.with_suffix(args.output.suffix + ".metadata.json")
    metadata_path.write_text(json.dumps(metadata, indent=2))

    print(f"Saved TorchScript model: {args.output}")
    print(f"Saved metadata: {metadata_path}")
    print(f"Expected C++ input: float32 [batch, {len(FEATURE_COLUMNS)}]")
    print(f"Expected C++ output: float32 [batch, {len(TARGET_COLUMNS)}]")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--csv",
        type=Path,
        default=Path("ml_training_data/lp_aware_packet_latency_training.csv"),
        help="Training CSV generated from CODES packet latency logs.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("ml_models/lp_aware_packet_latency_model.pt"),
        help="Output TorchScript .pt path.",
    )
    parser.add_argument("--epochs", type=int, default=50)
    parser.add_argument("--batch-size", type=int, default=4096)
    parser.add_argument("--hidden-dim", type=int, default=128)
    parser.add_argument("--dropout", type=float, default=0.0)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--weight-decay", type=float, default=1e-5)
    parser.add_argument("--val-fraction", type=float, default=0.1)
    parser.add_argument("--grad-clip", type=float, default=1.0)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--log-every", type=int, default=5)
    parser.add_argument(
        "--device",
        type=str,
        default="cpu",
        help="Use cpu for portable training/export; cuda is okay if available.",
    )

    return parser.parse_args()


if __name__ == "__main__":
    train(parse_args())
