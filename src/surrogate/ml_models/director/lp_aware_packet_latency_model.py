#!/usr/bin/env python3
"""
LP-aware TorchScript packet-latency model.

This is intentionally separate from the existing Torch-JIT model/exporter.
Use it only with:

    torch_jit_mode = lp-aware-single-static-model

C++ feature order:
    0  src_terminal
    1  dfdally_dest_terminal_id
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
"""

from __future__ import annotations

import argparse
from pathlib import Path

import torch
import torch.nn as nn


LP_AWARE_PACKET_FEATURES = [
    "src_terminal",
    "dfdally_dest_terminal_id",
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

TORCH_PACKET_FEATURE_COUNT = len(LP_AWARE_PACKET_FEATURES)


class LPAwarePacketLatencyModel(nn.Module):
    def __init__(self, hidden_dim: int = 64):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(TORCH_PACKET_FEATURE_COUNT, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, 2),
            nn.Softplus(),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # C++ passes float32 [batch, 12].
        x = x.to(torch.float32)
        return self.net(x)


def export_torchscript(output_path: Path, hidden_dim: int) -> None:
    model = LPAwarePacketLatencyModel(hidden_dim=hidden_dim)
    model.eval()

    example = torch.zeros((1, TORCH_PACKET_FEATURE_COUNT), dtype=torch.float32)

    with torch.no_grad():
        traced = torch.jit.trace(model, example)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    traced.save(str(output_path))
    print(f"saved LP-aware TorchScript model to {output_path}")
    print(f"input shape: [batch, {TORCH_PACKET_FEATURE_COUNT}], dtype=float32")
    print("output shape: [batch, 2]")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("lp_aware_packet_latency_model.pt"),
        help="Path to write the TorchScript model.",
    )
    parser.add_argument(
        "--hidden-dim",
        type=int,
        default=64,
        help="Hidden dimension for the prototype MLP.",
    )
    args = parser.parse_args()

    export_torchscript(args.output, args.hidden_dim)


if __name__ == "__main__":
    main()
