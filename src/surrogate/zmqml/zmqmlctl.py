#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import sys

import zmq


def request(endpoint: str, cmd: str, args: list[str], bindata: bytes = b"", **extra) -> dict:
    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.connect(endpoint)

    msg = {"cmd": cmd, "args": args}
    msg.update(extra)
    payload = json.dumps(msg).encode("utf-8") + b"\x00" + bindata

    socket.send(payload)
    response = socket.recv_json()

    socket.close()
    context.term()

    return response


def director_args(*values: object) -> list[str]:
    items = [str(value) for value in values]
    return [str(len(items)), *items]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Control client for zmqmlserver.py."
    )
    parser.add_argument(
        "--endpoint",
        default=os.environ.get("ZMQML_ENDPOINT", "tcp://localhost:5555"),
        help="ZeroMQ endpoint. Default: tcp://localhost:5555",
    )

    parser.add_argument(
        "--family",
        default=os.environ.get("ZMQML_SURROGATE_FAMILY", "iteration-time"),
        help="Director surrogate family. Default: iteration-time",
    )
    parser.add_argument(
        "--backend",
        default=os.environ.get("ZMQML_SURROGATE_BACKEND", "director"),
        help="Director surrogate backend. Default: director",
    )

    sub = parser.add_subparsers(dest="command", required=True)

    train = sub.add_parser("train", help="Train Director surrogate model from buffered records")
    train.add_argument(
        "--client",
        default="all",
        help="Client id to train, or 'all'. Default: all",
    )

    save = sub.add_parser("save", help="Save active Director surrogate model")
    save.add_argument("path", help="Output model path or directory")

    load = sub.add_parser("load", help="Load Director surrogate model")
    load.add_argument("path", help="Input model path or directory")

    status = sub.add_parser("status", help="Show Director surrogate model status")
    status.add_argument(
        "--client",
        default="all",
        help="Client id to inspect, or 'all'. Default: all",
    )

    records = sub.add_parser("load-records", help="Load Director surrogate records from CSV")
    records.add_argument("path", help="Input records CSV path")

    sub.add_parser("exit", help="Ask server to exit")

    args = parser.parse_args()

    director_meta = {
        "surrogate_family": args.family,
        "surrogate_backend": args.backend,
    }

    if args.command == "train":
        resp = request(
            args.endpoint,
            "director-request",
            director_args(args.client),
            operation="train-model",
            **director_meta,
        )
    elif args.command == "save":
        resp = request(
            args.endpoint,
            "director-request",
            director_args(args.path),
            operation="save-model",
            **director_meta,
        )
    elif args.command == "load":
        resp = request(
            args.endpoint,
            "director-request",
            director_args(args.path),
            operation="load-model",
            **director_meta,
        )
    elif args.command == "status":
        resp = request(
            args.endpoint,
            "director-request",
            director_args(args.client),
            operation="model-status",
            **director_meta,
        )
    elif args.command == "load-records":
        resp = request(
            args.endpoint,
            "director-request",
            director_args(args.path),
            operation="load-records-csv",
            **director_meta,
        )
    elif args.command == "exit":
        resp = request(args.endpoint, "exit", [])
    else:
        raise AssertionError(args.command)

    print(json.dumps(resp, indent=2, sort_keys=True))
    return 0 if resp.get("status") == "done" else 1


if __name__ == "__main__":
    raise SystemExit(main())
