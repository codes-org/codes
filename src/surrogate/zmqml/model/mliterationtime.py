from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Sequence

import math
import numpy as np
import torch
import torch.nn as nn


class IterationTimeMLP(nn.Module):
    def __init__(self, history_len: int, horizon: int, hidden_dim: int = 32):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(history_len, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, horizon),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.net(x)


@dataclass
class ClientIterationModel:
    history_len: int = 2
    horizon: int = 3
    hidden_dim: int = 32
    min_train_windows: int = 1
    max_epochs: int = 100
    lr: float = 1e-3
    device: str = "cpu"
    debug: bool = False

    records: List[float] = field(default_factory=list)
    model: IterationTimeMLP | None = None
    trained: bool = False

    def add_records(self, values: Iterable[float]) -> None:
        for value in values:
            value = float(value)
            if math.isfinite(value) and value > 0.0:
                self.records.append(value)

    def _make_dataset(self):
        values = np.asarray(self.records, dtype=np.float32)

        needed = self.history_len + self.horizon
        if len(values) < needed:
            return None, None

        xs = []
        ys = []

        for start in range(0, len(values) - needed + 1):
            x = values[start : start + self.history_len]
            y = values[start + self.history_len : start + needed]
            xs.append(x)
            ys.append(y)

        if len(xs) < self.min_train_windows:
            return None, None

        return (
            torch.tensor(np.asarray(xs), dtype=torch.float32, device=self.device),
            torch.tensor(np.asarray(ys), dtype=torch.float32, device=self.device),
        )

    def train_or_update(self) -> bool:
        x, y = self._make_dataset()
        if x is None or y is None:
            if self.debug:
                print(
                    f"[iteration-time model-train] skipped records={len(self.records)} "
                    f"history_len={self.history_len} horizon={self.horizon} "
                    f"min_train_windows={self.min_train_windows}",
                    flush=True,
                )
            return False

        if self.debug:
            print(
                f"[iteration-time model-train] training records={len(self.records)} "
                f"windows={int(x.shape[0])} history_len={self.history_len} "
                f"horizon={self.horizon}",
                flush=True,
            )

        if self.model is None:
            self.model = IterationTimeMLP(
                history_len=self.history_len,
                horizon=self.horizon,
                hidden_dim=self.hidden_dim,
            ).to(self.device)

        self.model.train()
        optimizer = torch.optim.Adam(self.model.parameters(), lr=self.lr)
        loss_fn = nn.MSELoss()

        for _ in range(self.max_epochs):
            optimizer.zero_grad()
            pred = self.model(x)
            loss = loss_fn(pred, y)
            loss.backward()
            optimizer.step()

        self.model.eval()
        self.trained = True
        return True

    def predict(self, requested_horizon: int | None = None) -> List[float]:
        requested_horizon = requested_horizon or self.horizon

        fallback = self._fallback_prediction(requested_horizon)

        if not self.trained or self.model is None:
            return fallback

        if len(self.records) < self.history_len:
            return fallback

        recent = np.asarray(self.records[-self.history_len :], dtype=np.float32)
        x = torch.tensor(recent[None, :], dtype=torch.float32, device=self.device)

        with torch.no_grad():
            y = self.model(x).detach().cpu().numpy()[0]

        if self.debug:
            print(
                f"[iteration-time model-predict] records={len(self.records)} "
                f"trained={int(self.trained)} recent={recent.tolist()} "
                f"raw={y.tolist()} fallback={fallback}",
                flush=True,
            )

        cleaned = []
        for value in y[:requested_horizon]:
            value = float(value)
            if not math.isfinite(value) or value <= 0.0:
                cleaned.append(fallback[len(cleaned)])
            else:
                cleaned.append(value)

        while len(cleaned) < requested_horizon:
            cleaned.append(fallback[len(cleaned)])

        return cleaned

    def _fallback_prediction(self, requested_horizon: int) -> List[float]:
        valid = [v for v in self.records if math.isfinite(v) and v > 0.0]

        if valid:
            value = float(np.median(valid[-32:]))
        else:
            value = 2_000_000.0

        return [value for _ in range(requested_horizon)]


class IterationTimeModelRegistry:
    def __init__(self, history_len: int = 4, horizon: int = 5, debug: bool = False):
        self.history_len = history_len
        self.horizon = horizon
        self.debug = bool(debug)
        self.models: Dict[int, ClientIterationModel] = {}

    def set_debug(self, enabled: bool) -> None:
        self.debug = bool(enabled)
        for model in self.models.values():
            model.debug = self.debug

    def get(self, client_id: int) -> ClientIterationModel:
        if client_id not in self.models:
            self.models[client_id] = ClientIterationModel(
                history_len=self.history_len,
                horizon=self.horizon,
                debug=self.debug,
            )
        return self.models[client_id]

    def add_records(self, client_id: int, values: Sequence[float]) -> None:
        model = self.get(client_id)
        model.add_records(values)
        model.train_or_update()

    def predict(self, client_id: int, horizon: int) -> List[float]:
        return self.get(client_id).predict(horizon)
