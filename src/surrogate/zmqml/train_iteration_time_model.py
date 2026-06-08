#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

from model.mliterationtime import IterationTimeModelRegistry


def parse_records_csv(path: Path) -> dict[int, list[float]]:
    records: dict[int, list[float]] = {}

    with path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        required = {"client", "value"}
        missing = required - set(reader.fieldnames or [])
        if missing:
            raise SystemExit(
                f"{path} is missing required columns: {sorted(missing)}"
            )

        for row in reader:
            try:
                client = int(row["client"])
                value = float(row["value"])
            except (TypeError, ValueError):
                continue

            if math.isfinite(value) and value > 0.0:
                records.setdefault(client, []).append(value)

    return records


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Train a pretrained ZeroMQ Director iteration-time model."
    )
    parser.add_argument(
        "--records-csv",
        required=True,
        help="CSV with columns: client,value",
    )
    parser.add_argument(
        "--output-model",
        required=True,
        help="Path to output .pt model file.",
    )
    parser.add_argument("--history-len", type=int, default=2)
    parser.add_argument("--horizon", type=int, default=3)
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()

    records_by_client = parse_records_csv(Path(args.records_csv))

    registry = IterationTimeModelRegistry(
        history_len=args.history_len,
        horizon=args.horizon,
        debug=args.debug,
    )

    trained_clients = 0
    for client, values in sorted(records_by_client.items()):
        model = registry.get(client)
        model.add_records(values)
        if model.train_or_update():
            trained_clients += 1

    if trained_clients == 0:
        raise SystemExit(
            "No client models were trained. Check that the pure PDES run "
            "produced enough records per client."
        )

    registry.save(args.output_model)

    print(
        f"saved {args.output_model} "
        f"clients={len(records_by_client)} trained_clients={trained_clients}"
    )


if __name__ == "__main__":
    main()
