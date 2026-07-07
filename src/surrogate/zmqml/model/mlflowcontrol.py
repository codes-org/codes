#!/usr/bin/env python3
"""Per-terminal interval-flow surrogate family for the ZeroMQ Director.

The server exposes one surrogate family name, ``flow-control``, but this module
keeps one trainable model per terminal/LP id.  Each terminal model learns only
from records whose ``lp_id`` matches that terminal, and inference for an LP uses
only that LP's model.

The model predicts raw per-flow offered load.  CODES/PDES remains responsible
for source queue capacity, grant capacity, drops, routing, and delivery.
"""

from __future__ import annotations

import csv
import io
import json
import math
import os
import pickle
from pathlib import Path
from typing import Any

import numpy as np


FLOW_RECORD_FIELDS = [
    "schema_version",
    "lp_id",
    "interval_id",
    "interval_seconds",
    "incoming_flow_key",
    "incoming_packets",
    "outgoing_flow_key",
    "raw_egress_packets",
    "prev_grant_ratio",
    "prev_queue_occupancy_ratio",
    "prev_dropped_packets",
    "prev_capacity_packets",
]


def _finite_float(value: Any, default: float = 0.0) -> float:
    try:
        out = float(value)
    except Exception:
        return default
    if not math.isfinite(out):
        return default
    return out


def _lp_key(value: Any) -> str:
    """Normalize LP ids so CSV strings, ints, and JSON numbers match."""
    text = str(value).strip()
    if text == "":
        return "unknown"
    try:
        num = float(text)
        if math.isfinite(num) and num.is_integer():
            return str(int(num))
    except Exception:
        pass
    return text


def _flow_key_reverse(flow_key: str) -> str | None:
    parts = str(flow_key).split("->", 1)
    if len(parts) != 2:
        return None
    left = parts[0].strip()
    right = parts[1].strip()
    if not left or not right:
        return None
    return f"{right}->{left}"


def _packets_from_flow_entry(entry: Any) -> float:
    if isinstance(entry, dict):
        return max(0.0, _finite_float(entry.get("packets", 0.0), 0.0))
    return max(0.0, _finite_float(entry, 0.0))


class TerminalFlowControlModel:
    """One ML model owned by one terminal/LP.

    For this first prototype, each terminal model is a small ridge-regression
    model per outgoing flow.  This still gives the desired ownership boundary:
    records and inference are isolated by LP id.  Extending this to one
    multi-output model per LP later only changes this class, not the server API.
    """

    def __init__(
        self,
        lp_id: str,
        *,
        ridge_alpha: float,
        min_rows_per_flow: int,
        default_prediction_packets: float,
        max_prediction_packets: float,
    ) -> None:
        self.lp_id = _lp_key(lp_id)
        self.ridge_alpha = max(0.0, float(ridge_alpha))
        self.min_rows_per_flow = max(1, int(min_rows_per_flow))
        self.default_prediction_packets = max(0.0, float(default_prediction_packets))
        self.max_prediction_packets = max(0.0, float(max_prediction_packets))

        self.rows: list[dict[str, Any]] = []
        self.coefficients: dict[str, np.ndarray] = {}
        self.flow_means: dict[str, float] = {}
        self.num_requests = 0
        self.last_predictions: dict[str, float] = {}

    @staticmethod
    def _features(
        *,
        incoming_packets: float,
        grant_ratio: float,
        queue_occupancy_ratio: float,
        dropped_packets: float,
        capacity_packets: float,
        interval_id: float,
    ) -> np.ndarray:
        return np.asarray(
            [
                1.0,
                max(0.0, incoming_packets),
                min(1.0, max(0.0, grant_ratio)),
                min(1.0, max(0.0, queue_occupancy_ratio)),
                math.log1p(max(0.0, dropped_packets)),
                math.log1p(max(0.0, capacity_packets)),
                float(interval_id),
            ],
            dtype=np.float64,
        )

    @classmethod
    def _row_features(cls, row: dict[str, Any]) -> np.ndarray:
        return cls._features(
            incoming_packets=_finite_float(row.get("incoming_packets"), 0.0),
            grant_ratio=_finite_float(row.get("prev_grant_ratio"), 1.0),
            queue_occupancy_ratio=_finite_float(row.get("prev_queue_occupancy_ratio"), 0.0),
            dropped_packets=_finite_float(row.get("prev_dropped_packets"), 0.0),
            capacity_packets=_finite_float(row.get("prev_capacity_packets"), 0.0),
            interval_id=_finite_float(row.get("interval_id"), 0.0),
        )

    @staticmethod
    def _target(row: dict[str, Any]) -> float:
        return max(0.0, _finite_float(row.get("raw_egress_packets"), 0.0))

    def add_row(self, row: dict[str, Any]) -> None:
        self.rows.append(row)

    def train_or_update(self) -> bool:
        by_flow: dict[str, list[dict[str, Any]]] = {}
        for row in self.rows:
            flow = str(row.get("outgoing_flow_key", "")).strip()
            if flow:
                by_flow.setdefault(flow, []).append(row)

        trained_any = False
        for flow, rows in sorted(by_flow.items()):
            if len(rows) < self.min_rows_per_flow:
                continue

            x = np.vstack([self._row_features(row) for row in rows])
            y = np.asarray([self._target(row) for row in rows], dtype=np.float64)
            self.flow_means[flow] = float(np.mean(y)) if len(y) else self.default_prediction_packets

            eye = np.eye(x.shape[1], dtype=np.float64)
            eye[0, 0] = 0.0  # Do not penalize intercept.
            try:
                beta = np.linalg.solve(x.T @ x + self.ridge_alpha * eye, x.T @ y)
            except np.linalg.LinAlgError:
                beta = np.linalg.pinv(x) @ y

            self.coefficients[flow] = beta.astype(np.float64)
            trained_any = True

        return trained_any

    def _predict_one(self, flow_key: str, features: np.ndarray) -> float:
        if flow_key in self.coefficients:
            raw = float(features @ self.coefficients[flow_key])
        elif flow_key in self.flow_means:
            raw = float(self.flow_means[flow_key])
        else:
            raw = self.default_prediction_packets

        raw = max(0.0, raw)
        if self.max_prediction_packets > 0.0:
            raw = min(raw, self.max_prediction_packets)
        return raw

    def predict(self, payload: dict[str, Any]) -> dict[str, float]:
        incoming = payload.get("incoming_flows", {}) or {}
        outgoing_feedback = payload.get("outgoing_feedback", {}) or {}
        outgoing_keys = payload.get("outgoing_flow_keys", []) or []
        interval_id = _finite_float(payload.get("interval_id"), 0.0)

        predictions: dict[str, float] = {}
        for key in outgoing_keys:
            flow_key = str(key).strip()
            if not flow_key:
                continue

            reverse_key = _flow_key_reverse(flow_key)
            incoming_packets = 0.0
            if reverse_key is not None and reverse_key in incoming:
                incoming_packets = _packets_from_flow_entry(incoming[reverse_key])
            elif incoming:
                incoming_packets = sum(_packets_from_flow_entry(v) for v in incoming.values())

            feedback = outgoing_feedback.get(flow_key, {}) or {}
            if not isinstance(feedback, dict):
                feedback = {}

            features = self._features(
                incoming_packets=incoming_packets,
                grant_ratio=_finite_float(feedback.get("grant_ratio"), 1.0),
                queue_occupancy_ratio=_finite_float(feedback.get("queue_occupancy_ratio"), 0.0),
                dropped_packets=_finite_float(feedback.get("dropped_packets"), 0.0),
                capacity_packets=_finite_float(feedback.get("capacity_packets"), 0.0),
                interval_id=interval_id,
            )
            predictions[flow_key] = self._predict_one(flow_key, features)

        self.num_requests += 1
        self.last_predictions = dict(predictions)
        return predictions

    def to_payload(self) -> dict[str, Any]:
        return {
            "lp_id": self.lp_id,
            "rows": self.rows,
            "coefficients": self.coefficients,
            "flow_means": self.flow_means,
            "num_requests": self.num_requests,
            "last_predictions": self.last_predictions,
        }

    @classmethod
    def from_payload(
        cls,
        payload: dict[str, Any],
        *,
        ridge_alpha: float,
        min_rows_per_flow: int,
        default_prediction_packets: float,
        max_prediction_packets: float,
    ) -> "TerminalFlowControlModel":
        model = cls(
            _lp_key(payload.get("lp_id", "unknown")),
            ridge_alpha=ridge_alpha,
            min_rows_per_flow=min_rows_per_flow,
            default_prediction_packets=default_prediction_packets,
            max_prediction_packets=max_prediction_packets,
        )
        model.rows = list(payload.get("rows", []))
        model.coefficients = {
            str(k): np.asarray(v, dtype=np.float64)
            for k, v in dict(payload.get("coefficients", {})).items()
        }
        model.flow_means = {
            str(k): float(v) for k, v in dict(payload.get("flow_means", {})).items()
        }
        model.num_requests = int(payload.get("num_requests", 0))
        model.last_predictions = {
            str(k): float(v) for k, v in dict(payload.get("last_predictions", {})).items()
        }
        return model

    def status(self) -> dict[str, Any]:
        flows = sorted(set(self.flow_means) | set(self.coefficients))
        return {
            "lp_id": self.lp_id,
            "rows": len(self.rows),
            "trained": bool(self.coefficients),
            "trained_flows": len(self.coefficients),
            "flows": flows,
            "requests": self.num_requests,
        }


class FlowControlSurrogateFamily:
    """Server-side family registry with one terminal model per LP id."""

    def __init__(
        self,
        *,
        ridge_alpha: float = 1.0,
        min_rows_per_flow: int = 1,
        default_prediction_packets: float = 1024.0,
        max_prediction_packets: float = 0.0,
    ) -> None:
        self.ridge_alpha = max(0.0, float(ridge_alpha))
        self.min_rows_per_flow = max(1, int(min_rows_per_flow))
        self.default_prediction_packets = max(0.0, float(default_prediction_packets))
        self.max_prediction_packets = max(0.0, float(max_prediction_packets))
        self.debug = False

        self.terminal_models: dict[str, TerminalFlowControlModel] = {}
        self.model_version = 0
        self.num_requests = 0

    def set_debug(self, enabled: bool) -> None:
        self.debug = bool(enabled)

    def _new_terminal_model(self, lp_id: str) -> TerminalFlowControlModel:
        return TerminalFlowControlModel(
            lp_id,
            ridge_alpha=self.ridge_alpha,
            min_rows_per_flow=self.min_rows_per_flow,
            default_prediction_packets=self.default_prediction_packets,
            max_prediction_packets=self.max_prediction_packets,
        )

    def _model_for_lp(self, lp_id: Any) -> TerminalFlowControlModel:
        key = _lp_key(lp_id)
        model = self.terminal_models.get(key)
        if model is None:
            model = self._new_terminal_model(key)
            self.terminal_models[key] = model
        return model

    @staticmethod
    def _normalize_record_row(row: dict[str, Any]) -> dict[str, Any] | None:
        flow = str(row.get("outgoing_flow_key", "")).strip()
        if not flow:
            return None
        out = {field: row.get(field, "") for field in FLOW_RECORD_FIELDS}
        out["lp_id"] = _lp_key(out.get("lp_id", "unknown"))
        out["outgoing_flow_key"] = flow
        return out

    def add_records_text(self, payload_text: str) -> int:
        text = (payload_text or "").strip()
        if not text:
            return 0

        loaded = 0
        if text.lstrip().startswith("{"):
            for line in text.splitlines():
                line = line.strip()
                if not line:
                    continue
                row = json.loads(line)
                if not isinstance(row, dict):
                    continue
                normalized = self._normalize_record_row(row)
                if normalized is None:
                    continue
                self._model_for_lp(normalized["lp_id"]).add_row(normalized)
                loaded += 1
            return loaded

        reader = csv.DictReader(io.StringIO(text))
        for row in reader:
            normalized = self._normalize_record_row(row)
            if normalized is None:
                continue
            self._model_for_lp(normalized["lp_id"]).add_row(normalized)
            loaded += 1
        return loaded

    def load_records_csv(self, path: str | Path) -> int:
        path = Path(path)
        loaded = 0
        files = sorted(path.rglob("*.csv")) if path.is_dir() else [path]
        for child in files:
            if not child.is_file():
                continue
            loaded += self.add_records_text(child.read_text())
        return loaded

    def train_or_update(self) -> bool:
        trained_any = False
        trained_lps: list[str] = []
        for lp_id, model in sorted(self.terminal_models.items()):
            if model.train_or_update():
                trained_any = True
                trained_lps.append(lp_id)

        if trained_any:
            self.model_version += 1

        if self.debug:
            print(
                f"[flow-control train] terminal_models={len(self.terminal_models)} "
                f"trained_lps={trained_lps}",
                flush=True,
            )
        return trained_any

    def predict(self, payload: dict[str, Any]) -> dict[str, float]:
        lp_id = _lp_key(payload.get("lp_id", "unknown"))
        model = self._model_for_lp(lp_id)
        predictions = model.predict(payload)
        self.num_requests += 1

        if self.debug:
            trained = bool(model.coefficients)
            print(
                f"[flow-control inference] lp={lp_id} trained={int(trained)} "
                f"interval={payload.get('interval_id')} predictions={predictions}",
                flush=True,
            )
        return predictions

    def predict_from_text(self, payload_text: str) -> dict[str, float]:
        payload_text = (payload_text or "").strip()
        if not payload_text:
            raise ValueError("missing flow-control JSON payload")
        payload = json.loads(payload_text)
        if not isinstance(payload, dict):
            raise ValueError("flow-control payload must be a JSON object")
        return self.predict(payload)

    def save(self, path: str | Path) -> None:
        path = Path(path)
        if path.parent:
            path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "format": "flow-control-per-terminal-linear-v1",
            "ridge_alpha": self.ridge_alpha,
            "min_rows_per_flow": self.min_rows_per_flow,
            "default_prediction_packets": self.default_prediction_packets,
            "max_prediction_packets": self.max_prediction_packets,
            "terminal_models": {
                lp_id: model.to_payload() for lp_id, model in sorted(self.terminal_models.items())
            },
            "model_version": self.model_version,
            "num_requests": self.num_requests,
        }
        with path.open("wb") as f:
            pickle.dump(payload, f)

    def load(self, path: str | Path) -> None:
        with Path(path).open("rb") as f:
            payload = pickle.load(f)

        self.ridge_alpha = float(payload.get("ridge_alpha", self.ridge_alpha))
        self.min_rows_per_flow = int(payload.get("min_rows_per_flow", self.min_rows_per_flow))
        self.default_prediction_packets = float(
            payload.get("default_prediction_packets", self.default_prediction_packets)
        )
        self.max_prediction_packets = float(payload.get("max_prediction_packets", self.max_prediction_packets))
        self.model_version = int(payload.get("model_version", self.model_version))
        self.num_requests = int(payload.get("num_requests", self.num_requests))

        # New format: one serialized model per LP.
        if "terminal_models" in payload:
            self.terminal_models = {
                _lp_key(lp_id): TerminalFlowControlModel.from_payload(
                    model_payload,
                    ridge_alpha=self.ridge_alpha,
                    min_rows_per_flow=self.min_rows_per_flow,
                    default_prediction_packets=self.default_prediction_packets,
                    max_prediction_packets=self.max_prediction_packets,
                )
                for lp_id, model_payload in dict(payload.get("terminal_models", {})).items()
            }
            return

        # Backward compatibility with the earlier one-object/per-flow prototype.
        legacy_rows = list(payload.get("rows", []))
        self.terminal_models = {}
        for row in legacy_rows:
            normalized = self._normalize_record_row(row)
            if normalized is not None:
                self._model_for_lp(normalized["lp_id"]).add_row(normalized)
        self.train_or_update()

    def status(self) -> dict[str, str]:
        lp_status = {lp_id: model.status() for lp_id, model in sorted(self.terminal_models.items())}
        trained_lps = [lp_id for lp_id, st in lp_status.items() if st["trained"]]
        trained_flow_labels: list[str] = []
        for lp_id, st in lp_status.items():
            for flow in st["flows"]:
                trained_flow_labels.append(f"{lp_id}:{flow}")

        total_rows = sum(int(st["rows"]) for st in lp_status.values())
        total_requests = sum(int(st["requests"]) for st in lp_status.values())

        return {
            "model_type": "per-terminal-linear-flow-control",
            "trained": "1" if trained_lps else "0",
            "rows": str(total_rows),
            "terminal_models": str(len(self.terminal_models)),
            "trained_terminal_models": str(len(trained_lps)),
            "trained_lps": ";".join(trained_lps),
            "trained_flows": str(len(trained_flow_labels)),
            "flows": ";".join(sorted(trained_flow_labels)),
            "requests": str(total_requests),
            "family_requests": str(self.num_requests),
            "model_version": str(self.model_version),
            "ridge_alpha": str(self.ridge_alpha),
            "min_rows_per_flow": str(self.min_rows_per_flow),
            "default_prediction_packets": str(self.default_prediction_packets),
        }


def flow_control_from_env() -> FlowControlSurrogateFamily:
    return FlowControlSurrogateFamily(
        ridge_alpha=float(os.environ.get("ZMQML_FLOW_CONTROL_RIDGE_ALPHA", "1.0")),
        min_rows_per_flow=int(os.environ.get("ZMQML_FLOW_CONTROL_MIN_ROWS_PER_FLOW", "1")),
        default_prediction_packets=float(
            os.environ.get("ZMQML_FLOW_CONTROL_DEFAULT_PACKETS", "1024")
        ),
        max_prediction_packets=float(os.environ.get("ZMQML_FLOW_CONTROL_MAX_PACKETS", "0")),
    )
