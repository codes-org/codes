from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional
import pickle

import numpy as np


def _as_positive_finite(values: List[float]) -> List[float]:
    out: List[float] = []
    for value in values:
        try:
            v = float(value)
        except (TypeError, ValueError):
            continue
        if np.isfinite(v) and v > 0.0:
            out.append(v)
    return out


@dataclass
class IterationTimeClientModel:
    """Per-client record holder.

    Training is global across all clients through the registry. This object keeps
    the old server-facing API intact: add_records(), train_or_update(), records,
    and trained.
    """

    client_id: int
    registry: "IterationTimeModelRegistry"
    app_id: int = -1
    records: List[float] = field(default_factory=list)

    @property
    def trained(self) -> bool:
        return bool(self.registry.trained)

    def set_app_id(self, app_id: int) -> None:
        try:
            self.app_id = int(app_id)
        except (TypeError, ValueError):
            self.app_id = -1

    def add_records(self, values: List[float]) -> None:
        self.records.extend(_as_positive_finite(values))
        self.registry.trained = False

    def train_or_update(self) -> bool:
        return self.registry.train_or_update()

    def _fallback_prediction(self, requested_horizon: int) -> List[float]:
        requested_horizon = max(1, int(requested_horizon))

        if not self.records:
            return [1.0 for _ in range(requested_horizon)]

        recent = self.records[-max(1, min(len(self.records), self.registry.history_len)) :]
        value = float(np.mean(recent))

        if not np.isfinite(value) or value <= 0.0:
            value = float(self.records[-1])

        if not np.isfinite(value) or value <= 0.0:
            value = 1.0

        return [value for _ in range(requested_horizon)]


class IterationTimeModelRegistry:
    """Global rich iteration-time regressor.

    The old model trained one sequence model per client. This version keeps
    per-client histories, but trains one shared ridge-regression model over all
    clients using richer features:

      app_id, client_id, iteration index, recent history, mean/std/trend

    That lets the model share signal across clients while still conditioning on
    app and client identity.
    """

    def __init__(
        self,
        history_len: int = 2,
        horizon: int = 3,
        ridge_alpha: float = 1.0,
        train_stride: int = 1,
    ):
        self.history_len = int(history_len)
        self.horizon = int(horizon)
        self.ridge_alpha = float(ridge_alpha)
        self.train_stride = max(1, int(train_stride))

        self.models: Dict[int, IterationTimeClientModel] = {}
        self.debug = False

        self.trained = False
        self.weights: Optional[np.ndarray] = None
        self.x_mean: Optional[np.ndarray] = None
        self.x_std: Optional[np.ndarray] = None
        self.y_mean: Optional[np.ndarray] = None
        self.y_std: Optional[np.ndarray] = None

        self.training_examples = 0
        self.model_version = 0

    def set_debug(self, enabled: bool) -> None:
        self.debug = bool(enabled)

    def get(self, client_id: int) -> IterationTimeClientModel:
        client_id = int(client_id)
        if client_id not in self.models:
            self.models[client_id] = IterationTimeClientModel(
                client_id=client_id,
                registry=self,
            )
        return self.models[client_id]

    def set_client_app_id(self, client_id: int, app_id: int) -> None:
        self.get(client_id).set_app_id(app_id)

    def add_records(
        self,
        client_id: int,
        values: List[float],
        app_id: Optional[int] = None,
    ) -> None:
        model = self.get(client_id)
        if app_id is not None:
            model.set_app_id(app_id)
        model.add_records(values)

    def _feature_vector(
        self,
        *,
        app_id: int,
        client_id: int,
        iteration: int,
        history: List[float],
    ) -> np.ndarray:
        hist = np.asarray(history, dtype=np.float64)
        if len(hist) != self.history_len:
            raise ValueError("history length mismatch")

        hist_mean = float(np.mean(hist))
        hist_std = float(np.std(hist))
        hist_min = float(np.min(hist))
        hist_max = float(np.max(hist))
        trend = float(hist[-1] - hist[0]) if len(hist) > 1 else 0.0

        # Numeric features only; no sklearn dependency.
        return np.asarray(
            [
                1.0,
                float(app_id),
                float(client_id),
                float(iteration),
                hist_mean,
                hist_std,
                hist_min,
                hist_max,
                trend,
                *[float(v) for v in hist],
            ],
            dtype=np.float64,
        )

    def _build_training_matrix(self) -> tuple[np.ndarray, np.ndarray]:
        x_rows: List[np.ndarray] = []
        y_rows: List[np.ndarray] = []

        h = self.history_len
        k = self.horizon

        for client_id in sorted(self.models):
            model = self.models[client_id]
            records = model.records

            if len(records) < h + k:
                continue

            for pos in range(h, len(records) - k + 1, self.train_stride):
                history = records[pos - h : pos]
                target = records[pos : pos + k]

                x_rows.append(
                    self._feature_vector(
                        app_id=model.app_id,
                        client_id=client_id,
                        iteration=pos,
                        history=history,
                    )
                )
                y_rows.append(np.asarray(target, dtype=np.float64))

        if not x_rows:
            return (
                np.empty((0, 0), dtype=np.float64),
                np.empty((0, self.horizon), dtype=np.float64),
            )

        return np.vstack(x_rows), np.vstack(y_rows)

    def train_or_update(self) -> bool:
        x, y = self._build_training_matrix()

        self.training_examples = int(x.shape[0])

        if x.shape[0] == 0:
            self.trained = False
            if self.debug:
                print(
                    "[IterationTimeModelRegistry] skipped training: no examples",
                    flush=True,
                )
            return False

        # Need at least a few examples to avoid fitting pure noise.
        if x.shape[0] < max(4, self.history_len + self.horizon):
            self.trained = False
            if self.debug:
                print(
                    "[IterationTimeModelRegistry] skipped training: "
                    f"examples={x.shape[0]}",
                    flush=True,
                )
            return False

        self.x_mean = np.mean(x, axis=0)
        self.x_std = np.std(x, axis=0)
        self.x_std[self.x_std == 0.0] = 1.0

        self.y_mean = np.mean(y, axis=0)
        self.y_std = np.std(y, axis=0)
        self.y_std[self.y_std == 0.0] = 1.0

        xz = (x - self.x_mean) / self.x_std
        yz = (y - self.y_mean) / self.y_std

        # Do not penalize intercept column too strongly.
        xtx = xz.T @ xz
        reg = self.ridge_alpha * np.eye(xtx.shape[0], dtype=np.float64)
        reg[0, 0] = 0.0

        xty = xz.T @ yz

        try:
            self.weights = np.linalg.solve(xtx + reg, xty)
        except np.linalg.LinAlgError:
            self.weights = np.linalg.lstsq(xtx + reg, xty, rcond=None)[0]

        self.trained = True
        self.model_version += 1

        if self.debug:
            print(
                "[IterationTimeModelRegistry] trained rich model "
                f"examples={self.training_examples} clients={len(self.models)} "
                f"features={x.shape[1]} horizon={self.horizon}",
                flush=True,
            )

        return True

    def _predict_once(self, client_id: int, history: List[float], iteration: int) -> np.ndarray:
        model = self.get(client_id)

        if (
            not self.trained
            or self.weights is None
            or self.x_mean is None
            or self.x_std is None
            or self.y_mean is None
            or self.y_std is None
            or len(history) < self.history_len
        ):
            return np.asarray(model._fallback_prediction(self.horizon), dtype=np.float64)

        hist = history[-self.history_len :]
        x = self._feature_vector(
            app_id=model.app_id,
            client_id=client_id,
            iteration=iteration,
            history=hist,
        )

        xz = (x - self.x_mean) / self.x_std
        yz = xz @ self.weights
        y = yz * self.y_std + self.y_mean

        # Keep predictions physically meaningful.
        fallback = model._fallback_prediction(self.horizon)
        cleaned = []
        for i, value in enumerate(y):
            value = float(value)
            if not np.isfinite(value) or value <= 0.0:
                value = float(fallback[min(i, len(fallback) - 1)])
            cleaned.append(max(value, 1.0))

        return np.asarray(cleaned, dtype=np.float64)

    def _global_fallback_prediction(self, requested_horizon: int) -> List[float]:
        requested_horizon = max(1, int(requested_horizon))

        recent_values: List[float] = []
        for model in self.models.values():
            if model.records:
                recent_values.extend(model.records[-max(1, self.history_len):])

        recent_values = _as_positive_finite(recent_values)

        if recent_values:
            value = float(np.median(np.asarray(recent_values, dtype=np.float64)))
        elif self.y_mean is not None and len(self.y_mean) > 0:
            value = float(self.y_mean[0])
        else:
            # Match the older iteration-time fallback scale instead of using 1.0.
            value = 2_000_000.0

        if not np.isfinite(value) or value <= 0.0:
            value = 2_000_000.0

        return [value for _ in range(requested_horizon)]

    def _global_fallback_prediction(self, requested_horizon: int) -> List[float]:
        requested_horizon = max(1, int(requested_horizon))

        recent_values: List[float] = []
        for model in self.models.values():
            if model.records:
                recent_values.extend(model.records[-max(1, self.history_len):])

        recent_values = _as_positive_finite(recent_values)

        if recent_values:
            value = float(np.median(np.asarray(recent_values, dtype=np.float64)))
        elif self.y_mean is not None and len(self.y_mean) > 0:
            value = float(self.y_mean[0])
        else:
            value = 2_000_000.0

        if not np.isfinite(value) or value <= 0.0:
            value = 2_000_000.0

        return [value for _ in range(requested_horizon)]

    def predict(self, client_id: int, requested_horizon: int | None = None) -> List[float]:
        client_id = int(client_id)
        requested_horizon = int(requested_horizon or self.horizon)
        requested_horizon = max(1, requested_horizon)

        model = self.get(client_id)

        if not model.records:
            fallback = self._global_fallback_prediction(requested_horizon)
            if self.debug:
                print(
                    "[IterationTimeModelRegistry] predict global-fallback "
                    f"client={client_id} requested_horizon={requested_horizon} "
                    f"trained={int(self.trained)} predictions={fallback}",
                    flush=True,
                )
            return fallback

        # Predict in chunks if requested_horizon > self.horizon.
        out: List[float] = []
        history = list(model.records)
        iteration = len(history)

        while len(out) < requested_horizon:
            pred = self._predict_once(client_id, history, iteration)
            for value in pred:
                if len(out) >= requested_horizon:
                    break
                out.append(float(value))
                history.append(float(value))
                iteration += 1

        if not out:
            out = self._global_fallback_prediction(requested_horizon)

        if self.debug:
            print(
                "[IterationTimeModelRegistry] predict "
                f"client={client_id} app={model.app_id} "
                f"requested_horizon={requested_horizon} trained={int(self.trained)} "
                f"predictions={out}",
                flush=True,
            )

        return out


    def save(self, path: str) -> None:
        out_path = Path(path)
        if out_path.parent:
            out_path.parent.mkdir(parents=True, exist_ok=True)

        state = {
            "format": "rich-global-ridge-v1",
            "history_len": self.history_len,
            "horizon": self.horizon,
            "ridge_alpha": self.ridge_alpha,
            "train_stride": self.train_stride,
            "trained": self.trained,
            "weights": self.weights,
            "x_mean": self.x_mean,
            "x_std": self.x_std,
            "y_mean": self.y_mean,
            "y_std": self.y_std,
            "training_examples": self.training_examples,
            "model_version": self.model_version,
            "clients": {
                client_id: {
                    "app_id": model.app_id,
                    "records": list(model.records),
                }
                for client_id, model in self.models.items()
            },
        }

        with out_path.open("wb") as f:
            pickle.dump(state, f)

    def load(self, path: str) -> None:
        in_path = Path(path)

        with in_path.open("rb") as f:
            state = pickle.load(f)

        self.history_len = int(state.get("history_len", self.history_len))
        self.horizon = int(state.get("horizon", self.horizon))
        self.ridge_alpha = float(state.get("ridge_alpha", self.ridge_alpha))
        self.train_stride = max(1, int(state.get("train_stride", self.train_stride)))

        self.trained = bool(state.get("trained", False))
        self.weights = state.get("weights")
        self.x_mean = state.get("x_mean")
        self.x_std = state.get("x_std")
        self.y_mean = state.get("y_mean")
        self.y_std = state.get("y_std")
        self.training_examples = int(state.get("training_examples", 0))
        self.model_version = int(state.get("model_version", 0))

        self.models = {}
        for raw_client_id, client_state in state.get("clients", {}).items():
            client_id = int(raw_client_id)
            model = self.get(client_id)
            model.set_app_id(int(client_state.get("app_id", -1)))
            model.records = _as_positive_finite(client_state.get("records", []))

        if self.debug:
            print(
                "[IterationTimeModelRegistry] loaded rich model "
                f"path={in_path} clients={len(self.models)} "
                f"trained={int(self.trained)} examples={self.training_examples}",
                flush=True,
            )
