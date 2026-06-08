#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import sys

import zmq


def request(endpoint: str, cmd: str, args: list[str], bindata: bytes = b"") -> dict:
    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.connect(endpoint)

    msg = {"cmd": cmd, "args": args}
    payload = json.dumps(msg).encode("utf-8") + b"\x00" + bindata

    socket.send(payload)
    response = socket.recv_json()

    socket.close()
    context.term()

    return response


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Control client for zmqmlserver.py."
    )
    parser.add_argument(
        "--endpoint",
        default=os.environ.get("ZMQML_ENDPOINT", "tcp://localhost:5555"),
        help="ZeroMQ endpoint. Default: tcp://localhost:5555",
    )

    sub = parser.add_subparsers(dest="command", required=True)

    train = sub.add_parser("train", help="Train iteration-time model from buffered records")
    train.add_argument(
        "--client",
        default="all",
        help="Client id to train, or 'all'. Default: all",
    )

    save = sub.add_parser("save", help="Save active iteration-time model")
    save.add_argument("path", help="Output .pt path")

    load = sub.add_parser("load", help="Load iteration-time model")
    load.add_argument("path", help="Input .pt path")

    status = sub.add_parser("status", help="Show iteration-time model status")
    status.add_argument(
        "--client",
        default="all",
        help="Client id to inspect, or 'all'. Default: all",
    )

    sub.add_parser("exit", help="Ask server to exit")

    args = parser.parse_args()

    if args.command == "train":
        resp = request(args.endpoint, "train-iteration-time-model", [args.client])
    elif args.command == "save":
        resp = request(args.endpoint, "save-iteration-time-model", [args.path])
    elif args.command == "load":
        resp = request(args.endpoint, "load-iteration-time-model", [args.path])
    elif args.command == "status":
        resp = request(args.endpoint, "iteration-time-model-status", [args.client])
    elif args.command == "exit":
        resp = request(args.endpoint, "exit", [])
    else:
        raise AssertionError(args.command)

    print(json.dumps(resp, indent=2, sort_keys=True))
    return 0 if resp.get("status") == "done" else 1


if __name__ == "__main__":
    raise SystemExit(main())
