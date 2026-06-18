from __future__ import annotations

import csv
import hashlib
import io
import json
import math
from pathlib import Path
from typing import Any, Iterable

import numpy as np
import torch
import torch.nn as nn


EVENT_TIME_SCHEMA_VERSION = 1

# Generic event-time schema:
#   A current PDES event schedules a target PDES event after target_delay.
#
# The core prediction task is:
#   current event + target event identity + nominal scheduling context -> target_delay
#
# Backend-specific context can be added later with columns named backend_feature_*.
CORE_FEATURE_COLUMNS = [
    "schema_version",
    "now",
    "current_lp_gid",
    "current_lp_type",
    "current_event_type",
    "target_lp_gid",
    "target_lp_type",
    "target_event_type",
    "nominal_delay",
]

TARGET_COLUMNS = [
    "target_delay",
    "delay",
    "scheduled_delay",
]

NON_FEATURE_COLUMNS = {
    "sample_id",
    "target_delay",
    "delay",
    "scheduled_delay",
    "target_timestamp",
    "backend",
    "record_kind",
}


class EventTimeMLP(nn.Module):
    def __init__(self, input_dim: int, hidden_dim: int = 64):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(input_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, 1),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.net(x).squeeze(-1)


class EventTimeModel:
    def __init__(
        self,
        min_rows: int = 32,
        max_epochs: int = 80,
        lr: float = 1e-3,
        hidden_dim: int = 64,
        device: str = "auto",
    ):
        self.min_rows = int(min_rows)
        self.max_epochs = int(max_epochs)
        self.lr = float(lr)
        self.hidden_dim = int(hidden_dim)
        if device is None or str(device).strip().lower() in ("", "auto"):
            self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        else:
            self.device = torch.device(device)

        self.rows: list[dict[str, Any]] = []
        self.feature_columns: list[str] = list(CORE_FEATURE_COLUMNS)

        self.model: EventTimeMLP | None = None
        self.x_mean: np.ndarray | None = None
        self.x_std: np.ndarray | None = None

        self.fallback_delay = 1.0
        self.trained = False
        self.training_examples = 0
        self.debug = False

    def set_debug(self, enabled: bool) -> None:
        self.debug = bool(enabled)

    @staticmethod
    def _clean_name(name: str) -> str:
        return str(name).strip().lstrip("#").strip()

    @staticmethod
    def _stable_string_number(value: str) -> float:
        digest = hashlib.sha256(value.encode("utf-8")).hexdigest()
        # Keep the value bounded so string-coded categories do not dominate.
        return float(int(digest[:8], 16) % 1000000) / 1000000.0

    @classmethod
    def _to_float(cls, value: Any, default: float = 0.0) -> float:
        if value is None:
            return default

        if isinstance(value, (int, float)):
            out = float(value)
            return out if math.isfinite(out) else default

        s = str(value).strip()
        if s == "":
            return default

        try:
            out = float(s)
            return out if math.isfinite(out) else default
        except ValueError:
            # Allows future rows to use names like T_SEND/R_ARRIVE/terminal/router
            # before we settle final numeric enum values.
            return cls._stable_string_number(s)

    @staticmethod
    def _looks_like_header(line: str) -> bool:
        return any(ch.isalpha() or ch == "_" for ch in line)

    def _target_from_row(self, row: dict[str, Any]) -> float | None:
        for col in TARGET_COLUMNS:
            if col in row:
                value = self._to_float(row[col], default=float("nan"))
                if math.isfinite(value) and value > 0.0:
                    return value
        return None

    def _rows_from_text(self, text: str) -> list[dict[str, Any]]:
        text = text.strip()
        if not text:
            return []

        if text[0] in "[{":
            payload = json.loads(text)
            if isinstance(payload, dict):
                return [payload]
            if isinstance(payload, list):
                return [row for row in payload if isinstance(row, dict)]
            return []

        lines = [line for line in text.splitlines() if line.strip()]
        if not lines:
            return []

        if not self._looks_like_header(lines[0]):
            # Headerless rows use the generic schema order.
            header = [
                "schema_version",
                "sample_id",
                "now",
                "current_lp_gid",
                "current_lp_type",
                "current_event_type",
                "target_lp_gid",
                "target_lp_type",
                "target_event_type",
                "nominal_delay",
                "target_delay",
            ]
            lines = [",".join(header)] + lines

        reader = csv.DictReader(io.StringIO("\n".join(lines)))
        if reader.fieldnames:
            reader.fieldnames = [self._clean_name(name) for name in reader.fieldnames]

        return list(reader)

    def _feature_columns_from_rows(self) -> list[str]:
        cols = list(CORE_FEATURE_COLUMNS)

        discovered = set()
        for row in self.rows:
            for key in row.keys():
                key = self._clean_name(key)
                if key in NON_FEATURE_COLUMNS:
                    continue
                if key in cols:
                    continue

                # Only backend-prefixed fields are admitted automatically.
                # This avoids accidentally turning packet identifiers or labels
                # into core event-time features.
                if key.startswith("backend_feature_"):
                    discovered.add(key)

        cols.extend(sorted(discovered))
        return cols

    def add_records_text(self, text: str) -> int:
        loaded = 0

        for raw in self._rows_from_text(text):
            row = {self._clean_name(k): v for k, v in raw.items()}

            target = self._target_from_row(row)
            if target is None:
                continue

            if "schema_version" not in row or str(row.get("schema_version", "")).strip() == "":
                row["schema_version"] = EVENT_TIME_SCHEMA_VERSION

            row["target_delay"] = target
            self.rows.append(row)
            loaded += 1

        if loaded > 0:
            targets = [
                self._to_float(row.get("target_delay"), default=float("nan"))
                for row in self.rows[-min(len(self.rows), 4096):]
            ]
            targets = [x for x in targets if math.isfinite(x) and x > 0.0]
            if targets:
                self.fallback_delay = float(np.median(np.asarray(targets, dtype=np.float64)))

            self.feature_columns = self._feature_columns_from_rows()

        return loaded

    def load_csv(self, path: str | Path) -> int:
        path = Path(path)

        if path.is_dir():
            total = 0
            for child in sorted(path.rglob("*")):
                if child.is_file() and child.suffix.lower() in (".csv", ".txt", ".log"):
                    total += self.load_csv(child)
            return total

        return self.add_records_text(path.read_text())

    def _make_arrays(self) -> tuple[np.ndarray, np.ndarray]:
        self.feature_columns = self._feature_columns_from_rows()

        x_rows = []
        y_rows = []

        for row in self.rows:
            target = self._to_float(row.get("target_delay"), default=float("nan"))
            if not math.isfinite(target) or target <= 0.0:
                continue

            x_rows.append([
                self._to_float(row.get(col, 0.0), default=0.0)
                for col in self.feature_columns
            ])
            y_rows.append(math.log1p(target))

        if not x_rows:
            return (
                np.zeros((0, len(self.feature_columns)), dtype=np.float32),
                np.zeros((0,), dtype=np.float32),
            )

        return (
            np.asarray(x_rows, dtype=np.float32),
            np.asarray(y_rows, dtype=np.float32),
        )

    def train_or_update(self) -> bool:
        x_np, y_np = self._make_arrays()
        self.training_examples = int(x_np.shape[0])

        if x_np.shape[0] < self.min_rows:
            if self.debug:
                print(
                    f"[event-time model-train] skipped rows={x_np.shape[0]} "
                    f"min_rows={self.min_rows}",
                    flush=True,
                )
            return False

        self.x_mean = x_np.mean(axis=0)
        self.x_std = x_np.std(axis=0)
        self.x_std[self.x_std < 1e-12] = 1.0

        x_np = (x_np - self.x_mean) / self.x_std

        x = torch.tensor(x_np, dtype=torch.float32, device=self.device)
        y = torch.tensor(y_np, dtype=torch.float32, device=self.device)

        self.model = EventTimeMLP(
            input_dim=len(self.feature_columns),
            hidden_dim=self.hidden_dim,
        ).to(self.device)

        opt = torch.optim.Adam(self.model.parameters(), lr=self.lr)
        loss_fn = nn.HuberLoss()

        self.model.train()
        for _ in range(self.max_epochs):
            opt.zero_grad()
            pred = self.model(x)
            loss = loss_fn(pred, y)
            loss.backward()
            opt.step()

        self.model.eval()
        self.trained = True

        if self.debug:
            print(
                f"[event-time model-train] trained rows={x_np.shape[0]} "
                f"features={len(self.feature_columns)} "
                f"fallback_delay={self.fallback_delay}",
                flush=True,
            )

        return True

    def predict_from_rows(self, rows: list[dict[str, Any]]) -> list[float]:
        if not rows:
            return [float(self.fallback_delay)]

        if not self.trained or self.model is None or self.x_mean is None or self.x_std is None:
            return [float(self.fallback_delay) for _ in rows]

        x_rows = []
        for row in rows:
            clean = {self._clean_name(k): v for k, v in row.items()}
            if "schema_version" not in clean:
                clean["schema_version"] = EVENT_TIME_SCHEMA_VERSION

            x_rows.append([
                self._to_float(clean.get(col, 0.0), default=0.0)
                for col in self.feature_columns
            ])

        x_np = np.asarray(x_rows, dtype=np.float32)
        x_np = (x_np - self.x_mean) / self.x_std

        x = torch.tensor(x_np, dtype=torch.float32, device=self.device)

        self.model.eval()
        with torch.no_grad():
            pred_log = self.model(x).detach().cpu().numpy()

        out = []
        for value in pred_log:
            delay = float(np.expm1(value))
            if not math.isfinite(delay) or delay <= 0.0:
                delay = float(self.fallback_delay)
            out.append(delay)

        return out

    def predict_from_text(self, text: str, requested_count: int = 1) -> list[float]:
        rows = self._rows_from_text(text) if text.strip() else []
        preds = self.predict_from_rows(rows)

        if not preds:
            preds = [float(self.fallback_delay)]

        while len(preds) < requested_count:
            preds.append(preds[-1])

        return preds[:requested_count]

    def status(self) -> dict[str, str]:
        return {
            "trained": str(int(self.trained)),
            "rows": str(len(self.rows)),
            "training_examples": str(self.training_examples),
            "feature_count": str(len(self.feature_columns)),
            "features": ",".join(self.feature_columns),
            "fallback_delay": str(float(self.fallback_delay)),
            "schema_version": str(EVENT_TIME_SCHEMA_VERSION),
        }

    def save(self, path: str | Path) -> None:
        payload = {
            "schema_version": EVENT_TIME_SCHEMA_VERSION,
            "rows": self.rows,
            "feature_columns": self.feature_columns,
            "min_rows": self.min_rows,
            "max_epochs": self.max_epochs,
            "lr": self.lr,
            "hidden_dim": self.hidden_dim,
            "trained": self.trained,
            "training_examples": self.training_examples,
            "fallback_delay": self.fallback_delay,
            "x_mean": self.x_mean,
            "x_std": self.x_std,
            "model_state_dict": self.model.state_dict() if self.model is not None else None,
        }
        torch.save(payload, str(path))

    def load(self, path: str | Path) -> None:
        try:
            payload = torch.load(str(path), map_location=self.device, weights_only=False)
        except TypeError:
            payload = torch.load(str(path), map_location=self.device)

        self.rows = list(payload.get("rows", []))
        self.feature_columns = list(payload.get("feature_columns", CORE_FEATURE_COLUMNS))
        self.min_rows = int(payload.get("min_rows", self.min_rows))
        self.max_epochs = int(payload.get("max_epochs", self.max_epochs))
        self.lr = float(payload.get("lr", self.lr))
        self.hidden_dim = int(payload.get("hidden_dim", self.hidden_dim))
        self.trained = bool(payload.get("trained", False))
        self.training_examples = int(payload.get("training_examples", 0))
        self.fallback_delay = float(payload.get("fallback_delay", self.fallback_delay))
        self.x_mean = payload.get("x_mean")
        self.x_std = payload.get("x_std")

        state = payload.get("model_state_dict")
        if state is not None:
            self.model = EventTimeMLP(
                input_dim=len(self.feature_columns),
                hidden_dim=self.hidden_dim,
            ).to(self.device)
            self.model.load_state_dict(state)
            self.model.eval()
            self.trained = True
        else:
            self.model = None
            self.trained = False
