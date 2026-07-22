from __future__ import annotations

import math
from typing import Iterable


SCHEMA_VERSION = "fluid-flow-wan-egress-v1"

REQUIRED_COLUMNS = {
    "schema_version",
    "interval",
    "egress_phase",
    "switch",
    "port",
    "target_is_terminal",
    "target_index",
    "interval_seconds",
    "interval_capacity_mbit",
    "capacity_used_mbit",
    "remaining_capacity_mbit",
    "eligible_data_mbit",
    "buffered_mbit",
    "staged_mbit",
    "shared_queued_mbit",
    "shared_buffer_mbit",
    "output_paused",
}


def _finite_float(row: dict[str, object], key: str, default: float = 0.0) -> float:
    try:
        value = float(row.get(key, default))
    except (TypeError, ValueError):
        return float(default)
    return value if math.isfinite(value) else float(default)


def _int_value(row: dict[str, object], key: str, default: int = 0) -> int:
    try:
        return int(float(row.get(key, default)))
    except (TypeError, ValueError):
        return int(default)


def validate_row(row: dict[str, object]) -> None:
    missing = sorted(REQUIRED_COLUMNS.difference(row.keys()))
    if missing:
        raise ValueError(f"missing fluid-flow-wan egress columns: {missing}")

    schema = str(row.get("schema_version", "")).strip()
    if schema != SCHEMA_VERSION:
        raise ValueError(
            f"unsupported fluid-flow-wan schema_version={schema!r}; "
            f"expected {SCHEMA_VERSION!r}"
        )

    phase = str(row.get("egress_phase", "")).strip().lower()
    if phase not in ("early", "late"):
        raise ValueError(
            f"unsupported fluid-flow-wan egress_phase={phase!r}; "
            "expected 'early' or 'late'"
        )


class FluidFlowWanSwitchStatisticalModel:
    """No-training phase-level egress predictor for one switch.

    Each SWITCH_EGRESS_EARLY or SWITCH_EGRESS_LATE event sends the currently
    eligible phase data and the physical link capacity remaining in the
    interval. The analytical model returns their minimum, or zero while the
    output link is paused. The model is read-only and deterministic, including
    when Time Warp re-executes an event.
    """

    def __init__(self, switch_id: int):
        self.switch_id = int(switch_id)
        self.inference_requests = 0

    def predict(self, rows: Iterable[dict[str, object]]) -> list[float]:
        predictions: list[float] = []
        for row in rows:
            validate_row(row)
            row_switch = _int_value(row, "switch", -1)
            if row_switch != self.switch_id:
                raise ValueError(
                    f"fluid-flow-wan row switch={row_switch} does not match "
                    f"model switch={self.switch_id}"
                )

            interval_capacity = max(
                0.0, _finite_float(row, "interval_capacity_mbit")
            )
            remaining_capacity = max(
                0.0, _finite_float(row, "remaining_capacity_mbit")
            )
            eligible_data = max(0.0, _finite_float(row, "eligible_data_mbit"))
            paused = _int_value(row, "output_paused") != 0

            if remaining_capacity > interval_capacity:
                remaining_capacity = interval_capacity

            prediction = 0.0 if paused else min(remaining_capacity, eligible_data)
            predictions.append(prediction)

        self.inference_requests += 1
        return predictions


class FluidFlowWanStatisticalRegistry:
    """One independent no-training statistical model per switch LP."""

    def __init__(self):
        self.models: dict[int, FluidFlowWanSwitchStatisticalModel] = {}

    def set_debug(self, enabled: bool) -> None:
        # Logging is centralized in zmqmlserver.py.
        _ = bool(enabled)

    def get(self, switch_id: int) -> FluidFlowWanSwitchStatisticalModel:
        switch_id = int(switch_id)
        if switch_id not in self.models:
            self.models[switch_id] = FluidFlowWanSwitchStatisticalModel(switch_id)
        return self.models[switch_id]

    def predict(
        self, switch_id: int, rows: Iterable[dict[str, object]]
    ) -> list[float]:
        return self.get(switch_id).predict(rows)

    def status(self) -> dict[str, str]:
        switch_ids = sorted(self.models)
        return {
            "model_count": str(len(switch_ids)),
            "switches": ";".join(
                f"{switch_id}:requests={self.models[switch_id].inference_requests}"
                for switch_id in switch_ids
            ),
        }
