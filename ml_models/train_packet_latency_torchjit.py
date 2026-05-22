from pathlib import Path
import argparse
import math
import random

import pandas as pd
import torch
from torch import nn
from torch.utils.data import DataLoader, TensorDataset, random_split



SEED = 1234
BATCH_SIZE = 4096
EPOCHS = 50
LR = 1e-3
WEIGHT_DECAY = 1e-4

random.seed(SEED)
torch.manual_seed(SEED)


class PacketLatencyModel(nn.Module):
    """
    TorchScript-compatible model for CODES torch-jit.C.

    C++ passes a LongTensor shaped [1, 4]:

        src_terminal
        dest_terminal
        packet_size
        is_there_another_pckt_in_queue

    This model returns a FloatTensor shaped [1, 2]:

        latency
        next_packet_delay
    """

    def __init__(self, n_src: int, n_dst: int, max_size: float, y_mean: torch.Tensor, y_std: torch.Tensor):
        super().__init__()

        emb_dim = 16
        hidden = 96

        self.src_emb = nn.Embedding(n_src, emb_dim)
        self.dst_emb = nn.Embedding(n_dst, emb_dim)

        self.register_buffer("max_size", torch.tensor(float(max_size), dtype=torch.float32))
        self.register_buffer("y_mean", y_mean.float())
        self.register_buffer("y_std", y_std.float())

        self.net = nn.Sequential(
            nn.Linear(emb_dim * 2 + 2, hidden),
            nn.ReLU(),
            nn.Linear(hidden, hidden),
            nn.ReLU(),
            nn.Linear(hidden, 2),
        )

        self.softplus = nn.Softplus()

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        src = x[:, 0].long()
        dst = x[:, 1].long()

        # Clamp defensively so an unseen id does not crash inference.
        src = torch.clamp(src, 0, self.src_emb.num_embeddings - 1)
        dst = torch.clamp(dst, 0, self.dst_emb.num_embeddings - 1)

        size = x[:, 2].float().unsqueeze(1)
        has_next = x[:, 3].float().unsqueeze(1)

        size_norm = size / torch.clamp(self.max_size, min=1.0)
        z = torch.cat(
            [
                self.src_emb(src),
                self.dst_emb(dst),
                size_norm,
                has_next,
            ],
            dim=1,
        )

        # Train in standardized target space, then unscale inside the scripted model.
        pred_scaled = self.net(z)
        pred = pred_scaled * self.y_std + self.y_mean

        # CODES expects positive latency/delay. Softplus prevents negative predictions.
        return self.softplus(pred)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Train a TorchScript packet-latency model for CODES torch-jit surrogate mode."
    )
    parser.add_argument(
        "--csv",
        required=True,
        type=Path,
        help="Path to merged packet-latency training CSV.",
    )
    parser.add_argument(
        "--out",
        required=True,
        type=Path,
        help="Output path for the saved TorchScript .pt model.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    csv_path = args.csv
    out_path = args.out

    if not csv_path.exists():
        raise SystemExit(f"Missing {csv_path}. Run the trace merge step first.")

    out_path.parent.mkdir(parents=True, exist_ok=True)

    df = pd.read_csv(csv_path)

    required = [
        "src_terminal",
        "dest_terminal",
        "size",
        "is_there_another_pckt_in_queue",
        "latency",
        "next_packet_delay",
    ]
    missing = [c for c in required if c not in df.columns]
    if missing:
        raise SystemExit(f"CSV is missing columns: {missing}")

    df = df.dropna(subset=required).copy()
    df = df[
        (df["latency"] > 0)
        & (df["next_packet_delay"] >= 0)
        & (df["src_terminal"] >= 0)
        & (df["dest_terminal"] >= 0)
        & (df["size"] > 0)
    ].copy()

    if len(df) < 100:
        raise SystemExit(f"Too few usable rows after filtering: {len(df)}")

    feature_cols = [
        "src_terminal",
        "dest_terminal",
        "size",
        "is_there_another_pckt_in_queue",
    ]
    target_cols = [
        "latency",
        "next_packet_delay",
    ]

    X = torch.tensor(df[feature_cols].to_numpy(), dtype=torch.long)
    y = torch.tensor(df[target_cols].to_numpy(), dtype=torch.float32)

    n_src = int(df["src_terminal"].max()) + 1
    n_dst = int(df["dest_terminal"].max()) + 1
    max_size = float(df["size"].max())

    y_mean = y.mean(dim=0)
    y_std = y.std(dim=0).clamp_min(1e-6)
    y_scaled = (y - y_mean) / y_std

    dataset = TensorDataset(X, y_scaled)

    n_val = max(1, int(0.1 * len(dataset)))
    n_train = len(dataset) - n_val

    generator = torch.Generator().manual_seed(SEED)
    train_ds, val_ds = random_split(dataset, [n_train, n_val], generator=generator)

    train_loader = DataLoader(train_ds, batch_size=BATCH_SIZE, shuffle=True)
    val_loader = DataLoader(val_ds, batch_size=BATCH_SIZE, shuffle=False)

    model = PacketLatencyModel(
        n_src=n_src,
        n_dst=n_dst,
        max_size=max_size,
        y_mean=y_mean,
        y_std=y_std,
    )

    optimizer = torch.optim.AdamW(model.parameters(), lr=LR, weight_decay=WEIGHT_DECAY)
    loss_fn = nn.SmoothL1Loss()

    best_val = math.inf
    best_state = None

    for epoch in range(EPOCHS):
        model.train()
        train_loss = 0.0

        for xb, yb_scaled in train_loader:
            pred = model(xb)
            y_true = yb_scaled * y_std + y_mean

            loss = loss_fn(pred, y_true)

            optimizer.zero_grad(set_to_none=True)
            loss.backward()
            optimizer.step()

            train_loss += loss.item() * xb.size(0)

        train_loss /= n_train

        model.eval()
        val_loss = 0.0
        with torch.no_grad():
            for xb, yb_scaled in val_loader:
                pred = model(xb)
                y_true = yb_scaled * y_std + y_mean
                loss = loss_fn(pred, y_true)
                val_loss += loss.item() * xb.size(0)

        val_loss /= n_val

        if val_loss < best_val:
            best_val = val_loss
            best_state = {k: v.detach().clone() for k, v in model.state_dict().items()}

        print(f"epoch={epoch:03d} train_loss={train_loss:.6f} val_loss={val_loss:.6f}")

    if best_state is not None:
        model.load_state_dict(best_state)

    model.eval()

    # Exact shape/dtype sanity check for src/surrogate/packet-latency-predictor/torch-jit.C
    dummy = torch.zeros((1, 4), dtype=torch.long)
    with torch.no_grad():
        out = model(dummy)

    if tuple(out.shape) != (1, 2):
        raise RuntimeError(f"Bad output shape: expected (1, 2), got {tuple(out.shape)}")

    if out.dtype != torch.float32:
        raise RuntimeError(f"Bad output dtype: expected float32, got {out.dtype}")

    scripted = torch.jit.script(model)
    scripted.save(str(out_path))

    print()
    print(f"Saved TorchScript model to: {out_path}")
    print(f"rows used: {len(df)}")
    print(f"n_src={n_src}, n_dst={n_dst}, max_size={max_size}")
    print(f"best_val_loss={best_val:.6f}")
    print(f"dummy_output={out.tolist()}")


if __name__ == "__main__":
    main()
