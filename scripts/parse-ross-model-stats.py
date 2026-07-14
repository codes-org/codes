#!/usr/bin/env python3
"""Reference parser for the ROSS model-level stats binaries produced by CODES runs.

Parses the two files written when a simulation runs with --model-stats:

  <stats-path>/ross-stats-model.bin         GVT (stats_type=1) and real-time (2) samples
  <stats-path>/ross-stats-analysis-lps.bin  virtual-time samples from the analysis LPs

and decodes the payloads of the ping pong tutorial server LP and the dragonfly-dally
terminal/router LPs. The full byte-level format is documented in
doc/model-stats-binary-format.md -- keep the two in sync.

Payloads are dispatched on their size, which depends on the network configuration; pass
--num-rails/--num-qos/--radix for non-default configs (defaults match the ping pong
tutorial config: 1 rail, 1 QoS level, radix 7). Unknown payload sizes are reported (and
dumped as hex with --dump-unknown) but do not fail the parse.

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


def build_dispatch(rails, qos, radix):
    """Map payload size -> (lp type name, decoder) for both files."""
    model = {
        44: ("svr", _decode_svr_model),
        72 + 16 * rails + 8 * rails * qos: (
            "terminal",
            lambda p: _decode_terminal_model(p, rails, qos),
        ),
        8 + 32 * radix: ("router", lambda p: _decode_router_model(p, radix)),
    }
    vt = {
        72: ("svr", _decode_svr_vt),
        128 + 16 * rails + 8 * rails * qos: (
            "terminal",
            lambda p: _decode_terminal_vt(p, rails, qos),
        ),
        72 + 32 * radix: ("router", lambda p: _decode_router_vt(p, radix)),
    }
    if len(model) != 3 or len(vt) != 3:
        raise FormatError(
            "payload sizes collide for this rails/qos/radix combination; "
            "cannot dispatch on size alone"
        )
    return model, vt


def parse_model_file(path, dispatch, rows, unknown):
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
        entry = dispatch.get(model_sz)
        if entry is None:
            unknown.append((path, model_sz, payload))
            continue
        lp_type, decoder = entry
        row = dict(meta)
        row.update(decoder(payload))
        rows.setdefault(lp_type, []).append(row)
    return n


def parse_vt_file(path, dispatch, rows, unknown):
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
        entry = dispatch.get(sample_sz)
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
    ap.add_argument("--csv-prefix", help="write <prefix>-<lptype>.csv files instead of stdout")
    ap.add_argument("--dump-unknown", action="store_true", help="hex-dump undecodable payloads")
    args = ap.parse_args()

    model_dispatch, vt_dispatch = build_dispatch(args.num_rails, args.num_qos, args.radix)

    rows = {}
    unknown = []
    total = 0
    try:
        for path in args.files:
            if "analysis-lps" in path:
                total += parse_vt_file(path, vt_dispatch, rows, unknown)
            else:
                total += parse_model_file(path, model_dispatch, rows, unknown)
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
            "against the run's network config (see doc/model-stats-binary-format.md)",
            file=sys.stderr,
        )
        if args.dump_unknown:
            for path, sz, payload in unknown:
                print(f"# {path} sz={sz}: {payload.hex()}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
