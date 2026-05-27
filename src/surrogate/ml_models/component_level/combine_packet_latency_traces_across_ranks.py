from pathlib import Path
import pandas as pd

indir = Path("packet-latency-trace")
files = sorted(indir.glob("packets-delay-gid=*.txt"))

if not files:
    raise SystemExit(f"No packet delay files found in {indir.resolve()}")

cols = [
    "src_terminal",
    "dest_terminal",
    "packet_size",
    "is_there_another_pckt_in_queue",
    "caller_lp_gid",
    "src_router_id",
    "src_group_id",
    "dst_router_id",
    "dst_group_id",
    "terminal_queue_length",
    "terminal_vc_occupancy",
    "processing_packet_delay",
    "travel_end_time_delta",
    "next_packet_delay",
    "packet_id",
    "is_surrogate_on",
    "is_predicted",
    "workload_injection",
    "start",
    "end",
    "latency",
]

frames = []
for f in files:
    df = pd.read_csv(f, comment="#", header=None)

    if df.shape[1] != len(cols):
        raise SystemExit(
            f"{f} has {df.shape[1]} columns, expected {len(cols)}. "
            "This usually means the data-output patch was not applied, "
            "or old packet-delay files are mixed with new ones."
        )

    df.columns = cols
    df["source_file"] = f.name
    frames.append(df)

out = pd.concat(frames, ignore_index=True)

# Keep only real high-fidelity rows.
out = out[(out["is_surrogate_on"] == 0) & (out["is_predicted"] == 0)]

# Keep rows with usable targets.
out = out[out["travel_end_time_delta"] > 0]
out = out[out["next_packet_delay"] > 0]

out_path = indir / "lp_aware_packet_latency_training.csv"
out.to_csv(out_path, index=False)

print(f"Input files: {len(files)}")
print(f"Training rows: {len(out)}")
print(f"Wrote: {out_path}")

print(df[[
    "src_terminal",
    "dest_terminal",
    "packet_size",
    "caller_lp_gid",
    "src_router_id",
    "dst_router_id",
    "terminal_queue_length",
    "terminal_vc_occupancy",
    "processing_packet_delay",
    "travel_end_time_delta",
    "next_packet_delay",
]].describe())
