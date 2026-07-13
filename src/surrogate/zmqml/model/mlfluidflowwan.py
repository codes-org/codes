"""Per-switch fluid-flow scheduling models for the ZeroMQ Director server.

The built-in trainer is intentionally an initial behavior-cloning baseline: it
learns the per-flowlet allocation fractions produced by a pure-PDES FIFO or
round-robin run. Vendor models may use any training procedure, but their
TorchScript files must implement the same four-tensor input contract and return
one nonnegative allocation value for each of 32 candidate flowlets.
"""

from __future__ import annotations

import csv
import math
from collections import defaultdict
from pathlib import Path
from typing import Any

import numpy as np
import torch
import torch.nn as nn


GLOBAL_FEATURE_COLUMNS = [
    "shared_buffer_mbit",
    "shared_queued_before_mbit",
    "shared_buffer_occupancy",
    "ingress_mbit_current",
    "ingress_mbit_previous",
    "egress_mbit_current",
    "egress_mbit_previous",
    "dropped_mbit_current",
    "dropped_mbit_previous",
    "total_active_flowlets",
]

PORT_FEATURE_COLUMNS = [
    "port",
    "target_index",
    "capacity_mbit",
    "port_queued_before_mbit",
    "active_before",
    "target_is_terminal",
]

CANDIDATE_FEATURE_COLUMNS = [
    "remaining_mbit",
    "age_intervals",
    "enqueue_age_intervals",
    "source_terminal",
    "destination_terminal",
    "candidate_rank",
]


class FluidFlowWanPolicy(nn.Module):
    """Score a bounded candidate set and return one allocation weight per flowlet."""

    def __init__(
        self,
        hidden_dim: int,
        global_mean: torch.Tensor,
        global_std: torch.Tensor,
        port_mean: torch.Tensor,
        port_std: torch.Tensor,
        candidate_mean: torch.Tensor,
        candidate_std: torch.Tensor,
    ) -> None:
        super().__init__()
        self.register_buffer("global_mean", global_mean)
        self.register_buffer("global_std", global_std)
        self.register_buffer("port_mean", port_mean)
        self.register_buffer("port_std", port_std)
        self.register_buffer("candidate_mean", candidate_mean)
        self.register_buffer("candidate_std", candidate_std)

        input_dim = (
            len(GLOBAL_FEATURE_COLUMNS)
            + len(PORT_FEATURE_COLUMNS)
            + len(CANDIDATE_FEATURE_COLUMNS)
        )
        self.scorer = nn.Sequential(
            nn.Linear(input_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, 1),
        )

    def forward(
        self,
        global_features: torch.Tensor,
        port_features: torch.Tensor,
        candidate_features: torch.Tensor,
        candidate_mask: torch.Tensor,
    ) -> torch.Tensor:
        global_norm = (global_features - self.global_mean) / self.global_std
        port_norm = (port_features - self.port_mean) / self.port_std
        candidate_norm = (candidate_features - self.candidate_mean) / self.candidate_std

        candidate_count = candidate_features.size(1)
        global_expanded = global_norm.unsqueeze(1).expand(-1, candidate_count, -1)
        port_expanded = port_norm.unsqueeze(1).expand(-1, candidate_count, -1)
        combined = torch.cat((global_expanded, port_expanded, candidate_norm), dim=-1)

        scores = self.scorer(combined).squeeze(-1)
        valid = candidate_mask > 0.5
        scores = scores.masked_fill(~valid, -1.0e9)
        weights = torch.softmax(scores, dim=-1)
        return weights * valid.to(weights.dtype)


def _to_float(value: Any, default: float = 0.0) -> float:
    try:
        out = float(value)
    except (TypeError, ValueError):
        return default
    return out if math.isfinite(out) else default


def _mean_std(values: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    mean = values.mean(axis=0).astype(np.float32)
    std = values.std(axis=0).astype(np.float32)
    std[std < 1.0e-6] = 1.0
    return mean, std


class FluidFlowWanModelRegistry:
    """Own training records and one TorchScript policy per logical switch."""

    def __init__(
        self,
        max_candidates: int = 32,
        min_examples: int = 16,
        hidden_dim: int = 64,
        max_epochs: int = 100,
        lr: float = 1.0e-3,
        device: str = "auto",
    ) -> None:
        self.max_candidates = int(max_candidates)
        self.min_examples = int(min_examples)
        self.hidden_dim = int(hidden_dim)
        self.max_epochs = int(max_epochs)
        self.lr = float(lr)
        if str(device).strip().lower() in ("", "auto"):
            self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        else:
            self.device = torch.device(device)

        self.rows: list[dict[str, str]] = []
        self.models: dict[int, torch.jit.ScriptModule] = {}
        self.training_examples: dict[int, int] = {}

    def load_records_csv(self, path: str | Path) -> int:
        path = Path(path)
        if not path.exists():
            raise FileNotFoundError(path)

        files = sorted(path.rglob("*.csv")) if path.is_dir() else [path]
        loaded = 0
        for child in files:
            with child.open(newline="") as f:
                reader = csv.DictReader(f)
                required = {
                    "decision_id",
                    "switch",
                    "candidate_rank",
                    "allocation_mbit",
                }
                if not reader.fieldnames or not required.issubset(set(reader.fieldnames)):
                    continue
                for row in reader:
                    record = dict(row)
                    record["_source_file"] = str(child.resolve())
                    self.rows.append(record)
                    loaded += 1
        return loaded

    def _examples_for_switch(self, switch_id: int):
        grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
        for row in self.rows:
            if int(_to_float(row.get("switch"), -1)) != switch_id:
                continue
            if str(row.get("scheduler", "")).strip().lower() == "ml":
                continue
            source_file = str(row.get("_source_file", ""))
            decision_id = str(row.get("decision_id", ""))
            grouped[f"{source_file}:{decision_id}"].append(row)

        examples = []
        for decision_id, rows in grouped.items():
            rows.sort(key=lambda row: int(_to_float(row.get("candidate_rank"), 0)))
            rows = rows[: self.max_candidates]
            if not rows:
                continue

            first = rows[0]
            allocations = np.asarray(
                [max(0.0, _to_float(row.get("allocation_mbit"))) for row in rows],
                dtype=np.float32,
            )
            allocation_sum = float(allocations.sum())
            if allocation_sum <= 1.0e-9:
                continue

            global_features = np.asarray(
                [_to_float(first.get(name)) for name in GLOBAL_FEATURE_COLUMNS],
                dtype=np.float32,
            )
            port_features = np.asarray(
                [_to_float(first.get(name)) for name in PORT_FEATURE_COLUMNS],
                dtype=np.float32,
            )
            candidates = np.zeros(
                (self.max_candidates, len(CANDIDATE_FEATURE_COLUMNS)), dtype=np.float32
            )
            mask = np.zeros((self.max_candidates,), dtype=np.float32)
            target = np.zeros((self.max_candidates,), dtype=np.float32)

            for idx, row in enumerate(rows):
                candidates[idx] = np.asarray(
                    [_to_float(row.get(name)) for name in CANDIDATE_FEATURE_COLUMNS],
                    dtype=np.float32,
                )
                mask[idx] = 1.0
                target[idx] = allocations[idx] / allocation_sum

            examples.append((decision_id, global_features, port_features, candidates, mask, target))
        return examples

    def train(self, target: str = "all") -> int:
        switch_ids = sorted(
            {
                int(_to_float(row.get("switch"), -1))
                for row in self.rows
                if int(_to_float(row.get("switch"), -1)) >= 0
            }
        )
        if target not in ("", "all", "*"):
            switch_ids = [int(target)]

        trained = 0
        previous_torch_threads = torch.get_num_threads()
        torch.set_num_threads(1)
        try:
            for switch_id in switch_ids:
                examples = self._examples_for_switch(switch_id)
                if len(examples) < self.min_examples:
                    continue

                global_np = np.stack([example[1] for example in examples])
                port_np = np.stack([example[2] for example in examples])
                candidate_np = np.stack([example[3] for example in examples])
                mask_np = np.stack([example[4] for example in examples])
                target_np = np.stack([example[5] for example in examples])

                valid_candidates = candidate_np[mask_np > 0.5]
                global_mean, global_std = _mean_std(global_np)
                port_mean, port_std = _mean_std(port_np)
                candidate_mean, candidate_std = _mean_std(valid_candidates)

                torch.manual_seed(12345 + switch_id)
                if torch.cuda.is_available():
                    torch.cuda.manual_seed_all(12345 + switch_id)

                model = FluidFlowWanPolicy(
                    hidden_dim=self.hidden_dim,
                    global_mean=torch.tensor(global_mean),
                    global_std=torch.tensor(global_std),
                    port_mean=torch.tensor(port_mean),
                    port_std=torch.tensor(port_std),
                    candidate_mean=torch.tensor(candidate_mean),
                    candidate_std=torch.tensor(candidate_std),
                ).to(self.device)

                global_tensor = torch.tensor(global_np, device=self.device)
                port_tensor = torch.tensor(port_np, device=self.device)
                candidate_tensor = torch.tensor(candidate_np, device=self.device)
                mask_tensor = torch.tensor(mask_np, device=self.device)
                target_tensor = torch.tensor(target_np, device=self.device)

                optimizer = torch.optim.Adam(model.parameters(), lr=self.lr)
                model.train()
                for _epoch in range(self.max_epochs):
                    optimizer.zero_grad(set_to_none=True)
                    weights = model(global_tensor, port_tensor, candidate_tensor, mask_tensor)
                    loss = -(
                        target_tensor * torch.log(weights.clamp_min(1.0e-9))
                    ).sum(dim=1).mean()
                    if not torch.isfinite(loss):
                        raise RuntimeError(
                            f"non-finite fluid-flow-wan training loss for switch {switch_id}"
                        )
                    loss.backward()
                    optimizer.step()

                model.eval()
                scripted = torch.jit.script(model.cpu())
                scripted = scripted.to(self.device)
                self.models[switch_id] = scripted
                self.training_examples[switch_id] = len(examples)
                trained += 1
        finally:
            torch.set_num_threads(previous_torch_threads)

        return trained

    def _validate_model_contract(
        self, model: torch.jit.ScriptModule, switch_id: int
    ) -> None:
        global_features = torch.zeros(
            (1, len(GLOBAL_FEATURE_COLUMNS)), dtype=torch.float32, device=self.device
        )
        port_features = torch.zeros(
            (1, len(PORT_FEATURE_COLUMNS)), dtype=torch.float32, device=self.device
        )
        candidate_features = torch.zeros(
            (1, self.max_candidates, len(CANDIDATE_FEATURE_COLUMNS)),
            dtype=torch.float32,
            device=self.device,
        )
        candidate_mask = torch.ones(
            (1, self.max_candidates), dtype=torch.float32, device=self.device
        )

        with torch.no_grad():
            result = model(
                global_features, port_features, candidate_features, candidate_mask
            )

        expected_shape = (1, self.max_candidates)
        if tuple(result.shape) != expected_shape:
            raise ValueError(
                f"switch-{switch_id}.pt returned shape {tuple(result.shape)}; "
                f"expected {expected_shape}"
            )
        if not torch.isfinite(result).all():
            raise ValueError(f"switch-{switch_id}.pt returned non-finite values")
        if (result < 0).any():
            raise ValueError(f"switch-{switch_id}.pt returned negative allocation values")
        if float(result.sum().item()) <= 1.0e-9:
            raise ValueError(f"switch-{switch_id}.pt returned only zero allocation values")

    def save(self, directory: str | Path) -> int:
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        for switch_id, model in sorted(self.models.items()):
            model = model.to("cpu")
            model.save(str(directory / f"switch-{switch_id}.pt"))
            self.models[switch_id] = model.to(self.device)
        return len(self.models)

    def load(self, directory: str | Path) -> int:
        directory = Path(directory)
        if not directory.is_dir():
            raise NotADirectoryError(directory)

        loaded: dict[int, torch.jit.ScriptModule] = {}
        for path in sorted(directory.glob("switch-*.pt")):
            switch_text = path.stem.removeprefix("switch-")
            try:
                switch_id = int(switch_text)
            except ValueError:
                continue
            model = torch.jit.load(str(path), map_location=self.device)
            model.eval()
            self._validate_model_contract(model, switch_id)
            loaded[switch_id] = model

        if not loaded:
            raise RuntimeError(f"no switch-<id>.pt models found in {directory}")
        self.models = loaded
        return len(loaded)

    def predict(
        self,
        switch_id: int,
        global_features: list[float],
        port_features: list[float],
        candidate_features: list[list[float]],
        candidate_mask: list[float],
    ) -> list[float]:
        if switch_id not in self.models:
            raise KeyError(f"no fluid-flow-wan model loaded for switch {switch_id}")
        if len(global_features) != len(GLOBAL_FEATURE_COLUMNS):
            raise ValueError("incorrect fluid-flow-wan global feature count")
        if len(port_features) != len(PORT_FEATURE_COLUMNS):
            raise ValueError("incorrect fluid-flow-wan port feature count")
        if len(candidate_features) != self.max_candidates:
            raise ValueError(
                f"expected {self.max_candidates} candidate rows, got {len(candidate_features)}"
            )
        if len(candidate_mask) != self.max_candidates:
            raise ValueError("incorrect fluid-flow-wan candidate mask count")
        for row in candidate_features:
            if len(row) != len(CANDIDATE_FEATURE_COLUMNS):
                raise ValueError("incorrect fluid-flow-wan candidate feature count")

        model = self.models[switch_id]
        with torch.no_grad():
            result = model(
                torch.tensor([global_features], dtype=torch.float32, device=self.device),
                torch.tensor([port_features], dtype=torch.float32, device=self.device),
                torch.tensor([candidate_features], dtype=torch.float32, device=self.device),
                torch.tensor([candidate_mask], dtype=torch.float32, device=self.device),
            )[0]
        return [float(value) for value in result.detach().cpu().tolist()]

    def status(self) -> dict[str, str]:
        switch_ids = sorted(self.models)
        return {
            "model_count": str(len(switch_ids)),
            "switches": ",".join(str(switch_id) for switch_id in switch_ids),
            "record_rows": str(len(self.rows)),
            "training_examples": ";".join(
                f"{switch_id}:{self.training_examples.get(switch_id, 0)}"
                for switch_id in switch_ids
            ),
            "max_candidates": str(self.max_candidates),
            "schema": "fluid-flow-wan-v1",
            "global_feature_count": str(len(GLOBAL_FEATURE_COLUMNS)),
            "port_feature_count": str(len(PORT_FEATURE_COLUMNS)),
            "candidate_feature_count": str(len(CANDIDATE_FEATURE_COLUMNS)),
        }
