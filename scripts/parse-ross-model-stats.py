#!/usr/bin/env python3
"""Reference parser for the ROSS model-level stats binaries produced by CODES runs.

Parses the two files written when a simulation runs with --model-stats:

  <stats-path>/ross-stats-model.bin         GVT (stats_type=1) and real-time (2) samples
  <stats-path>/ross-stats-analysis-lps.bin  virtual-time samples from the analysis LPs

and decodes the payloads of the ping pong tutorial server LP, the dragonfly-dally
terminal/router LPs, and the fluid-flow WAN terminal/switch LPs. The full byte-level format
is documented in doc/model-stats-binary-format.md -- keep the two in sync.

Payloads are dispatched on their size, which depends on the network configuration; pass
--num-rails/--num-qos/--radix for non-default dragonfly configs (defaults match the ping
pong tutorial config: 1 rail, 1 QoS level, radix 7). The fluid-flow WAN switch payload is
parametric in the topology's max port count, which is inferred from the payload size -- no
flag needed. Unknown payload sizes are reported (and dumped as hex with --dump-unknown) but
do not fail the parse.

Output: one CSV per LP type to stdout (or to <prefix>-<type>.csv with --csv-prefix).
Exits non-zero on framing errors or truncated input so tests can assert parseability.

stdlib only -- no numpy/pandas required.
"""

import argparse
import csv
import struct
import sys

MODEL_TYPE_FLAG = 3  # lp_metadata/sample_metadata flag value for model data

SAMPLE_METADATA = struct.Struct("<iidd")  # flag, sample_sz, ts, real_time
MODEL_METADATA = struct.Struct("<IIIfiI")  # peid, kpid, lpid, gvt, stats_type, model_sz
LP_METADATA = struct.Struct("<QQQddii")  # lpid, kpid, peid, ts, real_time, sample_sz, flag

STATS_TYPE_NAMES = {1: "gvt", 2: "rt", 3: "vt"}


class FormatError(Exception):
    pass


def _decode_svr_model(payload):
    f = struct.Struct("<Q4IqdI")
    vals = f.unpack_from(payload)
    return dict(
        zip(
            (
                "svr_id",
                "pings_sent",
                "pings_recvd",
                "pongs_sent",
                "pongs_recvd",
                "bytes_sent",
                "rtt_sum_ns",
                "rtt_count",
            ),
            vals,
        )
    )


def _decode_terminal_model(payload, rails, qos):
    off = 0
    row = {}
    (row["terminal_id"], row["fin_chunks"], row["data_size"], row["fin_hops"]) = (
        struct.unpack_from("<Q3q", payload, off)
    )
    off += 32
    (row["fin_chunks_time_ns"],) = struct.unpack_from("<d", payload, off)
    off += 8
    for i in range(rails):
        (row[f"busy_time_ns_r{i}"],) = struct.unpack_from("<d", payload, off)
        off += 8
    (row["packets_gen"], row["packets_fin"], row["min_fin"], row["nonmin_fin"]) = (
        struct.unpack_from("<4q", payload, off)
    )
    off += 32
    for i in range(rails):
        (row[f"stalled_chunks_r{i}"],) = struct.unpack_from("<Q", payload, off)
        off += 8
    for i in range(rails):
        for j in range(qos):
            (row[f"vc_occupancy_r{i}q{j}"],) = struct.unpack_from("<i", payload, off)
            off += 4
    for i in range(rails):
        for j in range(qos):
            (row[f"terminal_length_r{i}q{j}"],) = struct.unpack_from("<i", payload, off)
            off += 4
    return row


def _decode_router_model(payload, radix):
    off = 0
    row = {}
    (row["router_id"],) = struct.unpack_from("<Q", payload, off)
    off += 8
    for i in range(radix):
        row[f"busy_time_ns_p{i}"], row[f"link_traffic_p{i}"] = struct.unpack_from(
            "<dq", payload, off
        )
        off += 16
    for i in range(radix):
        (row[f"stalled_chunks_p{i}"],) = struct.unpack_from("<Q", payload, off)
        off += 8
    for i in range(radix):
        (row[f"vc_occupancy_sum_p{i}"],) = struct.unpack_from("<i", payload, off)
        off += 4
    for i in range(radix):
        (row[f"queued_count_p{i}"],) = struct.unpack_from("<i", payload, off)
        off += 4
    return row


def _decode_svr_vt(payload):
    vals = struct.Struct("<Q5qdqd").unpack_from(payload)
    return dict(
        zip(
            (
                "svr_id",
                "pings_sent",
                "pings_recvd",
                "pongs_sent",
                "pongs_recvd",
                "bytes_sent",
                "rtt_sum_ns",
                "rtt_count",
                "end_time",
            ),
            vals,
        )
    )


def _decode_terminal_vt(payload, rails, qos):
    row = {}
    (
        row["terminal_id"],
        row["fin_chunks"],
        row["data_size"],
        row["fin_hops"],
        row["fin_chunks_time_ns"],
        row["packets_gen"],
        row["packets_fin"],
        row["min_fin"],
        row["nonmin_fin"],
        row["end_time"],
        row["fwd_events"],
        row["rev_events"],
    ) = struct.unpack_from("<Qqqddqqqqdqq", payload, 0)
    off = 96 + 4 * 8  # scalars + 4 pointer slots (opaque on disk)
    for i in range(rails):
        (row[f"busy_time_ns_r{i}"],) = struct.unpack_from("<d", payload, off)
        off += 8
    for i in range(rails):
        (row[f"stalled_chunks_r{i}"],) = struct.unpack_from("<Q", payload, off)
        off += 8
    for i in range(rails):
        for j in range(qos):
            (row[f"vc_occupancy_r{i}q{j}"],) = struct.unpack_from("<i", payload, off)
            off += 4
    for i in range(rails):
        for j in range(qos):
            (row[f"terminal_length_r{i}q{j}"],) = struct.unpack_from("<i", payload, off)
            off += 4
    return row


def _decode_router_vt(payload, radix):
    row = {}
    (row["router_id"], row["end_time"], row["fwd_events"], row["rev_events"]) = (
        struct.unpack_from("<Qdqq", payload, 0)
    )
    off = 32 + 5 * 8  # scalars + 5 pointer slots (opaque on disk)
    for i in range(radix):
        (row[f"busy_time_ns_p{i}"],) = struct.unpack_from("<d", payload, off)
        off += 8
    for i in range(radix):
        (row[f"link_traffic_p{i}"],) = struct.unpack_from("<q", payload, off)
        off += 8
    for i in range(radix):
        (row[f"stalled_chunks_p{i}"],) = struct.unpack_from("<Q", payload, off)
        off += 8
    for i in range(radix):
        (row[f"vc_occupancy_sum_p{i}"],) = struct.unpack_from("<i", payload, off)
        off += 4
    for i in range(radix):
        (row[f"queued_count_p{i}"],) = struct.unpack_from("<i", payload, off)
        off += 4
    return row


# fluid-flow WAN payload geometry (doc/model-stats-binary-format.md). The terminal payloads
# are fixed size; the switch payloads carry per-port arrays of length max_ports, which is
# recovered from the payload size below.
FFW_TERMINAL_MODEL_SZ = 104
FFW_TERMINAL_VT_SZ = 176
FFW_SWITCH_MODEL_HEADER = 112
FFW_SWITCH_VT_HEADER = 120
FFW_SWITCH_VT_TRAILER = 72  # scalar part of the trailing rollback block
FFW_PORT_STRIDE = 48  # 4 f64 + 4 i32 per port, in both payloads
FFW_VT_PORT_TRAILER_STRIDE = 16  # 2 more f64 per port in the VT rollback block

FFW_TERMINAL_MODEL = struct.Struct("<Q4i6d4Q")
FFW_TERMINAL_MODEL_FIELDS = (
    "terminal_id",
    "attached_switch",
    "active_flows",
    "link_paused",
    "reserved",
    "generated_mbit",
    "sent_to_switch_mbit",
    "received_mbit",
    "source_backlog_mbit",
    "send_rate_sum_mbps",
    "pause_time_ns",
    "pause_frames_received",
    "resume_frames_received",
    "pause_updates_received",
    "rate_updates_received",
)
FFW_TERMINAL_VT = struct.Struct("<Q6d4Qd4i")
FFW_SWITCH_SCALARS = (
    "shared_buffer_capacity_mbit",
    "shared_buffer_occupancy_mbit",
    "buffered_residual_mbit",
    "sent_mbit",
    "delivered_local_mbit",
    "dropped_mbit",
    "pause_frames_sent",
    "resume_frames_sent",
    "pause_updates_sent",
    "pause_frames_received",
    "resume_frames_received",
    "pause_updates_received",
)
FFW_SWITCH_MODEL_HEAD = struct.Struct("<Q2i6d6Q")
FFW_SWITCH_VT_HEAD = struct.Struct("<Q6d6Qd2i")


def _decode_ffw_terminal_model(payload):
    vals = FFW_TERMINAL_MODEL.unpack_from(payload)
    row = dict(zip(FFW_TERMINAL_MODEL_FIELDS, vals))
    del row["reserved"]
    return row


def _decode_ffw_terminal_vt(payload):
    vals = FFW_TERMINAL_VT.unpack_from(payload)
    row = dict(
        zip(
            (
                "terminal_id",
                "generated_mbit",
                "sent_to_switch_mbit",
                "received_mbit",
                "source_backlog_mbit",
                "send_rate_sum_mbps",
                "pause_time_ns",
                "pause_frames_received",
                "resume_frames_received",
                "pause_updates_received",
                "rate_updates_received",
                "end_time",
                "attached_switch",
                "active_flows",
                "link_paused",
                "reserved",
            ),
            vals,
        )
    )
    del row["reserved"]
    # the trailing 64 bytes are rollback bookkeeping (the pre-sample snapshots); skip them
    return row


def _decode_ffw_port_block(payload, off, ports, row):
    """Decode the per-port arrays shared by both fluid-flow WAN switch payloads."""
    for name in (
        "port_capacity_mbit_per_interval",
        "port_queued_mbit",
        "port_sent_mbit",
        "port_pause_time_ns",
    ):
        for i in range(ports):
            (row[f"{name}_p{i}"],) = struct.unpack_from("<d", payload, off)
            off += 8
    for name in (
        "port_target_is_terminal",
        "port_target_index",
        "port_output_paused",
        "port_queued_segments",
    ):
        for i in range(ports):
            (row[f"{name}_p{i}"],) = struct.unpack_from("<i", payload, off)
            off += 4
    return off


def _decode_ffw_switch_model(payload, ports):
    vals = FFW_SWITCH_MODEL_HEAD.unpack_from(payload, 0)
    row = dict(zip(("switch_id", "num_ports", "reserved") + FFW_SWITCH_SCALARS, vals))
    del row["reserved"]
    _decode_ffw_port_block(payload, FFW_SWITCH_MODEL_HEADER, ports, row)
    return row


def _decode_ffw_switch_vt(payload, ports):
    vals = FFW_SWITCH_VT_HEAD.unpack_from(payload, 0)
    row = dict(
        zip(
            ("switch_id",) + FFW_SWITCH_SCALARS + ("end_time", "num_ports", "reserved"),
            vals,
        )
    )
    del row["reserved"]
    _decode_ffw_port_block(payload, FFW_SWITCH_VT_HEADER, ports, row)
    # the trailing block (72 B + 16 B per port) is rollback bookkeeping; skip it
    return row


def ffw_switch_ports(size, kind):
    """Recover fluid-flow WAN max_ports from a switch payload size (None if not one)."""
    if kind == "model":
        rest, stride = size - FFW_SWITCH_MODEL_HEADER, FFW_PORT_STRIDE
    else:
        rest = size - FFW_SWITCH_VT_HEADER - FFW_SWITCH_VT_TRAILER
        stride = FFW_PORT_STRIDE + FFW_VT_PORT_TRAILER_STRIDE
    if rest <= 0 or rest % stride:
        return None
    return rest // stride


def _sized_dispatch(entries, kind):
    table = {}
    for size, name, decoder in entries:
        if size in table:
            raise FormatError(
                f"{kind} payload sizes collide at {size} bytes ({table[size][0]} vs {name}) "
                "for this rails/qos/radix combination; cannot dispatch on size alone"
            )
        table[size] = (name, decoder)
    return table


def build_dispatch(rails, qos, radix, family="auto"):
    """Map payload size -> (lp type name, decoder) for both files.

    A single simulation only ever contains one model family, but dispatch is by payload
    size alone, so an unusual --num-rails/--num-qos/--radix can make a dragonfly size
    coincide with a fluid-flow WAN one. That is reported as an error; pass --model-family
    to drop the family the run did not use.
    """
    dragonfly = family in ("auto", "dragonfly")
    ffw = family in ("auto", "fluid-flow-wan")
    model = []
    vt = []
    if dragonfly:
        model += [
            (44, "svr", _decode_svr_model),
            (
                72 + 16 * rails + 8 * rails * qos,
                "terminal",
                lambda p: _decode_terminal_model(p, rails, qos),
            ),
            (8 + 32 * radix, "router", lambda p: _decode_router_model(p, radix)),
        ]
        vt += [
            (72, "svr", _decode_svr_vt),
            (
                128 + 16 * rails + 8 * rails * qos,
                "terminal",
                lambda p: _decode_terminal_vt(p, rails, qos),
            ),
            (72 + 32 * radix, "router", lambda p: _decode_router_vt(p, radix)),
        ]
    if ffw:
        model.append((FFW_TERMINAL_MODEL_SZ, "ffw-terminal", _decode_ffw_terminal_model))
        vt.append((FFW_TERMINAL_VT_SZ, "ffw-terminal", _decode_ffw_terminal_vt))
    return _sized_dispatch(model, "model"), _sized_dispatch(vt, "vt")


def resolve_payload(size, dispatch, kind, family="auto"):
    """Exact-size lookup first, then the parametric fluid-flow WAN switch payload."""
    entry = dispatch.get(size)
    if entry is not None:
        return entry
    if family not in ("auto", "fluid-flow-wan"):
        return None
    ports = ffw_switch_ports(size, kind)
    if ports is None:
        return None
    if kind == "model":
        return ("ffw-switch", lambda p: _decode_ffw_switch_model(p, ports))
    return ("ffw-switch", lambda p: _decode_ffw_switch_vt(p, ports))


def parse_model_file(path, dispatch, rows, unknown, family="auto"):
    """Parse ross-stats-model.bin (GVT + RT records)."""
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    n = 0
    while off < len(data):
        if len(data) - off < SAMPLE_METADATA.size + MODEL_METADATA.size:
            raise FormatError(f"{path}: truncated record header at byte {off}")
        flag, sample_sz, ts, real_time = SAMPLE_METADATA.unpack_from(data, off)
        if flag != MODEL_TYPE_FLAG or sample_sz != MODEL_METADATA.size:
            raise FormatError(
                f"{path}: bad sample_metadata at byte {off} "
                f"(flag={flag}, sample_sz={sample_sz}) -- lost framing"
            )
        off += SAMPLE_METADATA.size
        peid, kpid, lpid, gvt, stats_type, model_sz = MODEL_METADATA.unpack_from(data, off)
        off += MODEL_METADATA.size
        if len(data) - off < model_sz:
            raise FormatError(f"{path}: truncated payload at byte {off}")
        payload = data[off : off + model_sz]
        off += model_sz
        n += 1
        meta = {
            "stats_type": STATS_TYPE_NAMES.get(stats_type, str(stats_type)),
            "ts": ts,
            "real_time": real_time,
            "gvt": gvt,
            "peid": peid,
            "kpid": kpid,
            "lpid": lpid,
        }
        entry = resolve_payload(model_sz, dispatch, "model", family)
        if entry is None:
            unknown.append((path, model_sz, payload))
            continue
        lp_type, decoder = entry
        row = dict(meta)
        row.update(decoder(payload))
        rows.setdefault(lp_type, []).append(row)
    return n


def parse_vt_file(path, dispatch, rows, unknown, family="auto"):
    """Parse ross-stats-analysis-lps.bin (virtual-time records)."""
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    n = 0
    while off < len(data):
        if len(data) - off < LP_METADATA.size:
            raise FormatError(f"{path}: truncated record header at byte {off}")
        lpid, kpid, peid, ts, real_time, sample_sz, flag = LP_METADATA.unpack_from(data, off)
        if flag != MODEL_TYPE_FLAG:
            raise FormatError(
                f"{path}: bad lp_metadata at byte {off} (flag={flag}) -- lost framing"
            )
        off += LP_METADATA.size
        if len(data) - off < sample_sz:
            raise FormatError(f"{path}: truncated payload at byte {off}")
        payload = data[off : off + sample_sz]
        off += sample_sz
        n += 1
        meta = {
            "stats_type": "vt",
            "ts": ts,
            "real_time": real_time,
            "gvt": "",
            "peid": peid,
            "kpid": kpid,
            "lpid": lpid,
        }
        entry = resolve_payload(sample_sz, dispatch, "vt", family)
        if entry is None:
            unknown.append((path, sample_sz, payload))
            continue
        lp_type, decoder = entry
        row = dict(meta)
        row.update(decoder(payload))
        rows.setdefault(lp_type, []).append(row)
    return n


def write_csv(rows, csv_prefix):
    for lp_type, entries in sorted(rows.items()):
        entries.sort(key=lambda r: (r["ts"], r["lpid"]))
        # union of keys across entries, preserving first-seen order (vt rows have extras)
        fields = []
        for r in entries:
            for k in r:
                if k not in fields:
                    fields.append(k)
        if csv_prefix:
            out = open(f"{csv_prefix}-{lp_type}.csv", "w", newline="")
        else:
            out = sys.stdout
            out.write(f"# --- {lp_type} ({len(entries)} records) ---\n")
        writer = csv.DictWriter(out, fieldnames=fields, restval="")
        writer.writeheader()
        writer.writerows(entries)
        if csv_prefix:
            out.close()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("files", nargs="+", help="ross-stats-model.bin and/or ross-stats-analysis-lps.bin")
    ap.add_argument("--num-rails", type=int, default=1, help="dragonfly-dally num_rails (default 1)")
    ap.add_argument("--num-qos", type=int, default=1, help="dragonfly-dally num_qos_levels (default 1)")
    ap.add_argument("--radix", type=int, default=7, help="dragonfly-dally router radix (default 7)")
    ap.add_argument(
        "--model-family",
        choices=("auto", "dragonfly", "fluid-flow-wan"),
        default="auto",
        help="restrict payload dispatch to one model family; only needed when the "
        "dragonfly dimensions make a payload size collide (default: auto)",
    )
    ap.add_argument("--csv-prefix", help="write <prefix>-<lptype>.csv files instead of stdout")
    ap.add_argument("--dump-unknown", action="store_true", help="hex-dump undecodable payloads")
    args = ap.parse_args()

    try:
        model_dispatch, vt_dispatch = build_dispatch(
            args.num_rails, args.num_qos, args.radix, args.model_family
        )
    except FormatError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    rows = {}
    unknown = []
    total = 0
    try:
        for path in args.files:
            if "analysis-lps" in path:
                total += parse_vt_file(path, vt_dispatch, rows, unknown, args.model_family)
            else:
                total += parse_model_file(path, model_dispatch, rows, unknown, args.model_family)
    except (FormatError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    write_csv(rows, args.csv_prefix)

    counts = {t: len(e) for t, e in sorted(rows.items())}
    print(f"# parsed {total} records: {counts}, unknown payloads: {len(unknown)}", file=sys.stderr)
    if unknown:
        sizes = sorted({sz for (_, sz, _) in unknown})
        print(
            f"# undecoded payload sizes {sizes} -- check --num-rails/--num-qos/--radix "
            "and --model-family against the run's network config "
            "(see doc/model-stats-binary-format.md)",
            file=sys.stderr,
        )
        if args.dump_unknown:
            for path, sz, payload in unknown:
                print(f"# {path} sz={sz}: {payload.hex()}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
