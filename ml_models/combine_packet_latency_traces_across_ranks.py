from pathlib import Path
import pandas as pd

trace_dir = Path("packet-latency-trace")
out = Path("packet_latency_train.csv")

cols = [
    "src_terminal",
    "dest_terminal",
    "packet_id",
    "is_surrogate_on",
    "is_predicted",
    "size",
    "workload_injection",
    "next_packet_delay",
    "start",
    "end",
    "latency",
    "is_there_another_pckt_in_queue",
]

files = sorted(trace_dir.glob("packets-delay-gid=*.txt"))
if not files:
    raise SystemExit(f"No packet trace files found under {trace_dir}")

dfs = []
for f in files:
    df = pd.read_csv(f, comment="#", header=None, names=cols)
    df["rank_file"] = f.name
    dfs.append(df)

df = pd.concat(dfs, ignore_index=True)

# For training the current torch-jit predictor, use only real PDES packets.
df = df[
    (df["is_surrogate_on"] == 0) &
    (df["is_predicted"] == 0) &
    (df["end"] >= 0) &
    (df["latency"] > 0)
].copy()

# Current torch-jit model predicts [latency, next_packet_delay].
# Drop rows where next_packet_delay is unavailable.
df = df[df["next_packet_delay"] >= 0].copy()

df.to_csv(out, index=False)

print(f"Wrote {out}")
print(f"rows={len(df)}")

print(df[[
    "src_terminal",
    "dest_terminal",
    "size",
    "is_there_another_pckt_in_queue",
    "latency",
    "next_packet_delay",
]].describe())
