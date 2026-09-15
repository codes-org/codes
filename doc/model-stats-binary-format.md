# Binary format of the ROSS model-level stats output

This document specifies the on-disk layout of the model-level data that CODES models emit
through the ROSS instrumentation layer, so external tools (e.g. NetMaestro) can parse it
without reading the C source. It covers the payloads written by the ping pong tutorial's
server LP (`doc/example/tutorial-synthetic-ping-pong.c`), the dragonfly-dally terminal
and router LPs (`src/networks/model-net/dragonfly-dally.cxx`), and the fluid-flow WAN
terminal and switch LPs (`src/network-workloads/model-net-fluid-flow-wan.cxx`).

For how a model *registers* these callbacks, see [codes-vis-readme.md](codes-vis-readme.md).
A reference parser lives at `scripts/parse-ross-model-stats.py` (stdlib-only Python).

## Producing the files

Model-level stats are enabled purely from the ROSS command line — no config-file changes:

| flag | meaning |
|---|---|
| `--model-stats=1` | sample at GVT, every `--num-gvt`-th GVT (default 10) |
| `--model-stats=2` | sample at wall-clock intervals of `--rt-interval` ms |
| `--model-stats=3` | sample at virtual-time intervals of `--vt-interval` (until `--vt-samp-end`) via analysis LPs. `--vt-samp-end` defaults to the simulation end time — set it explicitly for models with a far-future `g_tw_ts_end` (the ping pong tutorial uses 24 h of virtual time; the fluid-flow WAN models use a year), or the analysis LPs keep scheduling empty sample events long after the traffic ends |
| `--model-stats=4` | all of the above |
| `--event-trace=N` | event tracing (separate `evtrace` file; not covered here, see the ROSS docs) |
| `--stats-path=<dir>` | output directory, default `stats-output` |
| `--stats-prefix=<name>` | filename prefix, default `ross-stats` |
| `--buffer-size=<bytes>` | per-collection-type circular buffer size |

Example (ping pong tutorial, from the build directory):

```
mpirun -np 3 doc/example/tutorial-synthetic-ping-pong --sync=3 --num_messages=100 \
    --model-stats=1 --num-gvt=8 --stats-path=model-stats-out -- doc/example/tutorial-ping-pong.conf
```

Example (fluid-flow WAN trace replay; the model reads its topology/trace from the working
directory, so run it from a directory holding copies of `doc/example/fluid-flow-wan-*`):

```
mpirun -np 1 <builddir>/src/model-net-fluid-flow-wan-trace-traffic --sync=1 \
    --model-stats=3 --vt-interval=1e8 --vt-samp-end=1.7e10 --stats-path=model-stats-out \
    -- fluid-flow-wan-trace-traffic.yaml
```

Outputs:

- GVT and real-time samples → `<stats-path>/<prefix>-model.bin`
- virtual-time samples → `<stats-path>/<prefix>-analysis-lps.bin`

If `<stats-path>` already exists, ROSS appends `-<pid>-<timestamp>` to the directory name
instead of reusing it — scripts should run in a fresh directory or glob for the suffix.

## General properties (both files)

- Everything is written in the **native byte order and type sizes of the machine that ran
  the simulation** — in practice little-endian LP64 (`long` = 8 bytes). There is no
  endianness marker in the file.
- Records are packed back to back with **no alignment padding or framing between records**;
  read until EOF. All record and payload layouts below have no internal padding holes.
- In parallel runs each PE flushes its own circular buffer at GVT into one shared file via
  MPI-IO, so records are **not globally sorted by time**; sort on the timestamp field after
  parsing.
- If a PE's buffer fills up, further records are **silently dropped** (a warning with the
  missed byte count is printed at the end of the run); increase `--buffer-size` if that
  happens.
- Delta-style fields cover the interval since the previous sample of the same mode. The
  stretch between the last sample and the end of the run is **not emitted**, so sums over
  samples slightly undercount end-of-run totals.
- Under optimistic sync (`--sync=3`), real-time-mode samples are taken mid-window, so a
  later rollback can make individual RT deltas **slightly negative** (sums across samples
  stay correct). GVT and VT samples only cover committed/rolled-forward time and don't
  have this artifact; prefer GVT mode for clean per-interval data.
- With `--model-stats=4`, the GVT and RT modes share one accumulator (ROSS invokes the
  same collection callback for both), so each delta lands in whichever sample fires first
  — GVT records and RT records each hold a partial view and only their **combined** sums
  are complete. The virtual-time path keeps its own accumulator, so VT records are
  complete on their own. Don't sum GVT+VT (that double-counts); pick one mode's records
  per analysis.

## `ross-stats-model.bin` — GVT and real-time samples

Each record is: `sample_metadata` (24 B) + `model_metadata` (24 B) + `model_sz` payload
bytes. The structs come from ROSS (`core/instrumentation/st-instrumentation.h`).

`sample_metadata` — Python struct format `<iidd`:

| off | type | field | notes |
|---|---|---|---|
| 0 | i32 | flag | always 3 (`MODEL_TYPE`) — validate this |
| 4 | i32 | sample_sz | always 24 (= sizeof model_metadata) |
| 8 | f64 | ts | virtual time of the sampled LP — use this for time axes |
| 16 | f64 | real_time | wall-clock seconds since the run started |

`model_metadata` — Python struct format `<IIIfiI`:

| off | type | field | notes |
|---|---|---|---|
| 0 | u32 | peid | PE (MPI rank) |
| 4 | u32 | kpid | KP |
| 8 | u32 | lpid | **global LP id** (truncated to 32 bits) |
| 12 | f32 | gvt | GVT at collection; low precision — prefer `ts` |
| 16 | i32 | stats_type | 1 = GVT sample, 2 = real-time sample |
| 20 | u32 | model_sz | payload size in bytes that follows |

Dispatch the payload decoder on `model_sz` (all three payload sizes are distinct for any
one run configuration) and cross-check with the id in the first payload field.

### Payload: ping pong server LP (`nw-lp`) — 44 bytes, `<Q4IqdI`

All counters are deltas over the sampling interval.

| off | type | field |
|---|---|---|
| 0 | u64 | svr_id (server-relative id) |
| 8 | u32 | pings_sent |
| 12 | u32 | pings_recvd |
| 16 | u32 | pongs_sent |
| 20 | u32 | pongs_recvd |
| 24 | i64 | bytes_sent (payload bytes handed to model-net) |
| 32 | f64 | rtt_sum (ns; sum of PING→PONG round trips completed) |
| 40 | u32 | rtt_count (mean RTT for the interval = rtt_sum / rtt_count) |

### Payload: dragonfly-dally terminal — `72 + 16R + 8RQ` bytes

R = `num_rails`, Q = `num_qos_levels` from the network config (tutorial config: R=1, Q=1
→ **96 bytes**). Fields are deltas unless marked *snapshot* (instantaneous value at the
sampling instant).

| off | type | field |
|---|---|---|
| 0 | u64 | terminal_id (terminal-relative id) |
| 8 | i64 | fin_chunks (chunks delivered to this terminal) |
| 16 | i64 | data_size (payload bytes delivered) |
| 24 | i64 | fin_hops (total hops over delivered chunks) |
| 32 | f64 | fin_chunks_time (ns; total end-to-end latency of delivered chunks — mean chunk latency = fin_chunks_time / fin_chunks) |
| 40 | f64 ×R | busy_time (ns the injection link spent busy, per rail) |
| 40+8R | i64 | packets_gen (packets injected) |
| 48+8R | i64 | packets_fin (packets fully delivered) |
| 56+8R | i64 | min_fin (delivered chunks that took a minimal route) |
| 64+8R | i64 | nonmin_fin (delivered chunks that took a non-minimal route) |
| 72+8R | u64 ×R | stalled_chunks (chunks that could not inject due to full VC, per rail) |
| 72+16R | i32 ×R·Q | vc_occupancy — *snapshot*, bytes occupying the injection VCs, rail-major (`[rail][qos]`) |
| 72+16R+4RQ | i32 ×R·Q | terminal_length — *snapshot*, bytes queued at the NIC, rail-major |

Note: while a terminal is frozen by the network surrogate (surrogate configs only), the
pointer-backed fields (busy_time, stalled_chunks, vc_occupancy, terminal_length) read as
zeros.

### Payload: dragonfly-dally router — `8 + 32·radix` bytes

radix = ports per router (tutorial config: 7 → **232 bytes**). Port order is fixed by the
dragonfly connection manager: intra-group ports, then global ports, then terminal ports
(see `codes/network-manager/dragonfly-network-manager.h`); mapping port index → link
requires the topology/connection files used by the run.

| off | type | field |
|---|---|---|
| 0 | u64 | router_id (router-relative id) |
| 8 | (f64,i64) ×radix | interleaved per port: busy_time (ns), link_traffic (bytes forwarded) |
| 8+16·radix | u64 ×radix | stalled_chunks (chunks diverted to the overflow queue, per port) |
| 8+24·radix | i32 ×radix | vc_occupancy_sum — *snapshot*, bytes in all VCs of the port |
| 8+28·radix | i32 ×radix | queued_count — *snapshot*, bytes in the port's overflow queue |

### Payload: fluid-flow WAN terminal (`fluid-flow-wan-terminal-lp`) — 104 bytes, `<Q4i6d4Q`

Emitted by both `model-net-fluid-flow-wan-random-traffic` and
`model-net-fluid-flow-wan-trace-traffic` (one source file, two workload front ends; the
instrumentation is identical).

**All data volumes are raw Mbit.** The model's CSV logs and finalize lines optionally
rescale to Gbit when the topology YAML was written in Gb units (`to_output_data_unit()`);
that rescaling is deliberately *not* applied to these payloads, so a consumer never has to
know which unit the config used.

Fields marked *delta* cover the interval since the previous sample of the same mode and are
computed as `lifetime_total - lifetime_total_at_previous_sample`, so they telescope exactly
(their sum over all samples is the lifetime total at the last sample). Fields marked
*gauge* are instantaneous values read from LP state at the sampling instant.

Because the deltas are differences of lifetime totals, a rollback after a sample was taken
shows up as a **single slightly negative GVT/RT delta**, not as lost volume — the running
sum stays exact. Virtual-time samples cannot do even that: their reverse callback restores
the previous snapshot bit-exactly (see the trailing rollback block below), so VT deltas are
always non-negative.

| off | type | field |
|---|---|---|
| 0 | u64 | terminal_id (terminal-relative id) |
| 8 | i32 | attached_switch (switch-relative id of the switch this terminal hangs off) |
| 12 | i32 | active_flows — *gauge*, source flows with `remaining_source_mbit > EPS` (the predicate the model's own `active_source_flows` finalize line uses) |
| 16 | i32 | link_paused — *gauge*, 1 while this terminal's access link is PAUSE-asserted |
| 20 | i32 | reserved (always 0) |
| 24 | f64 | generated_mbit — *delta*, offered/generated volume admitted to source backlog |
| 32 | f64 | sent_to_switch_mbit — *delta*, volume injected onto the access link |
| 40 | f64 | received_mbit — *delta*, volume delivered to this terminal |
| 48 | f64 | source_backlog_mbit — *gauge*, Σ `remaining_source_mbit` over active flows |
| 56 | f64 | send_rate_sum_mbps — *gauge*, Σ `current_send_rate_mbps` over active flows (tracks max-min convergence) |
| 64 | f64 | pause_time_ns — *delta* of `total_pause_time_ns`; only accrues when a PAUSE ends, so it is 0 for intervals spent entirely paused and jumps on resume |
| 72 | u64 | pause_frames_received — *delta* |
| 80 | u64 | resume_frames_received — *delta* |
| 88 | u64 | pause_updates_received — *delta*, PAUSE/RESUME state changes actually applied |
| 96 | u64 | rate_updates_received — *delta*, rate-feedback updates that changed state |

### Payload: fluid-flow WAN switch (`fluid-flow-wan-switch-lp`) — `112 + 48P` bytes

`P` = **max_ports**, the maximum port count over all switches in the loaded topology,
fixed once at registration time so every switch LP emits the same record size; a switch
with fewer ports zero-fills the trailing entries (its own count is in `num_ports`, and the
per-port `port_target_index` entries are −1 beyond it). Recover it from the record's
payload size: `P = (model_sz − 112) / 48` (must divide exactly and be ≥ 1).

Port order is the order `switch_init()` builds it: inter-switch links first, in topology
YAML order, then the switch's terminals. Volumes are raw Mbit, as above.

| off | type | field |
|---|---|---|
| 0 | u64 | switch_id (switch-relative id) |
| 8 | i32 | num_ports (this switch's real port count; ≤ P) |
| 12 | i32 | reserved (always 0) |
| 16 | f64 | shared_buffer_capacity_mbit — static, the switch's shared buffer size |
| 24 | f64 | shared_buffer_occupancy_mbit — *gauge*, total queued Mbit across all output queues; this is exactly the quantity the PAUSE high/low watermarks act on |
| 32 | f64 | buffered_residual_mbit — *gauge*, unsent arrival residual admitted to the physical buffer |
| 40 | f64 | sent_mbit — *delta*, total egress volume (= Σ of the per-port `port_sent_mbit` below) |
| 48 | f64 | delivered_local_mbit — *delta*, egress volume handed to locally attached terminals |
| 56 | f64 | dropped_mbit — *delta* |
| 64 | u64 | pause_frames_sent — *delta* |
| 72 | u64 | resume_frames_sent — *delta* |
| 80 | u64 | pause_updates_sent — *delta*, PAUSE/RESUME state changes pushed upstream |
| 88 | u64 | pause_frames_received — *delta* |
| 96 | u64 | resume_frames_received — *delta* |
| 104 | u64 | pause_updates_received — *delta* |
| 112 | f64 ×P | port_capacity_mbit_per_interval — static, link capacity per fluid interval |
| 112+8P | f64 ×P | port_queued_mbit — *gauge*, volume queued on that output port |
| 112+16P | f64 ×P | port_sent_mbit — *delta*, egress volume on that port (per-link utilization) |
| 112+24P | f64 ×P | port_pause_time_ns — *delta* of that output link's `output_link_total_pause_time_ns` |
| 112+32P | i32 ×P | port_target_is_terminal — static, 1 if the port faces a terminal, 0 if a switch |
| 112+36P | i32 ×P | port_target_index — static, peer terminal or switch relative index (−1 for unused entries) |
| 112+40P | i32 ×P | port_output_paused — *gauge*, 1 while that output link is PAUSE-asserted |
| 112+44P | i32 ×P | port_queued_segments — *gauge*, number of non-empty queued fluid segments on the port |

For the 3-switch example topology (`doc/example/fluid-flow-wan-topology.yaml`, max 2 links
+ 2 terminals) P = 4 → **304 bytes**.

## `ross-stats-analysis-lps.bin` — virtual-time samples

Each record is: `lp_metadata` (48 B) + `sample_sz` payload bytes. Only LPs with a non-zero
VT sample size appear (in a ping pong run: the server, terminal and router LPs).

`lp_metadata` (`core/instrumentation/ross-lps/analysis-lp.h`) — Python struct format
`<QQQddii` (note the id order differs from `model_metadata`):

| off | type | field |
|---|---|---|
| 0 | u64 | lpid (global LP id, full width) |
| 8 | u64 | kpid |
| 16 | u64 | peid |
| 24 | f64 | ts (virtual time of the sampling point) |
| 32 | f64 | real_time |
| 40 | i32 | sample_sz (payload bytes that follow) |
| 44 | i32 | flag (always 3 = model) |

The payloads are the C sample structs written verbatim, so they **contain pointer members
whose on-disk bytes are meaningless** — parsers must skip those 8-byte slots (marked below).
The variable-length arrays the pointers refer to are appended directly after the struct,
in the listed order. Dispatch on `sample_sz` (72 / 152 / 296 at the tutorial config).

### Payload: ping pong server `struct svr_sample` — 72 bytes, `<Qqqqqqdqd`

svr_id, pings_sent, pings_recvd, pongs_sent, pongs_recvd, bytes_sent (all i64 deltas
except u64 svr_id), rtt_sum (f64, ns), rtt_count (i64), end_time (f64 virtual time of the
sample). No pointers, no appended arrays.

### Payload: dally terminal `struct dfly_cn_ross_sample` — `128 + 16R + 8RQ` bytes

| off | type | field |
|---|---|---|
| 0 | u64 | terminal_id |
| 8 | i64 | fin_chunks |
| 16 | i64 | data_size |
| 24 | f64 | fin_hops (double here, unlike the GVT/RT payload) |
| 32 | f64 | fin_chunks_time (ns) |
| 40 | i64 | packets_gen |
| 48 | i64 | packets_fin |
| 56 | i64 | min_fin |
| 64 | i64 | nonmin_fin |
| 72 | f64 | end_time (virtual time of the sample) |
| 80 | i64 | fwd_events (forward events processed) |
| 88 | i64 | rev_events (events rolled back) |
| 96 | 4×8 B | pointer slots — skip |
| 128 | f64 ×R | busy_time (ns per rail) |
| 128+8R | u64 ×R | stalled_chunks per rail |
| 128+16R | i32 ×R·Q | vc_occupancy *snapshot*, rail-major |
| 128+16R+4RQ | i32 ×R·Q | terminal_length *snapshot*, rail-major |

Tutorial config: **152 bytes**.

### Payload: dally router `struct dfly_router_ross_sample` — `72 + 32·radix` bytes

| off | type | field |
|---|---|---|
| 0 | u64 | router_id |
| 8 | f64 | end_time |
| 16 | i64 | fwd_events |
| 24 | i64 | rev_events |
| 32 | 5×8 B | pointer slots — skip |
| 72 | f64 ×radix | busy_time (ns per port) |
| 72+8·radix | i64 ×radix | link_traffic (bytes per port) |
| 72+16·radix | u64 ×radix | stalled_chunks per port |
| 72+24·radix | i32 ×radix | vc_occupancy_sum *snapshot* |
| 72+28·radix | i32 ×radix | queued_count *snapshot* |

Tutorial config: **296 bytes**.

### Fluid-flow WAN VT payloads — the trailing rollback block

Unlike the dragonfly samples, the two fluid-flow WAN virtual-time payloads are **not C
structs written verbatim**: they are assembled byte by byte, so there are no padding holes
and no pointer slots to skip. Their scalar content is the same as the GVT/RT payloads above
plus an `end_time` (f64, `tw_now()` at the sampling instant, per the convention the other
models use).

Both VT payloads end with a **trailing rollback block**. Virtual-time sampling runs inside
an analysis-LP event that can be rolled back, so each sample records the delta bookkeeping
that existed *before* it was taken; the reverse callback restores it bit-exactly from these
bytes. It is internal state, not measurement — **consumers should ignore it**. It is kept
at the tail so a parser can decode the useful prefix and stop.

### Payload: fluid-flow WAN terminal VT — 176 bytes

Useful prefix `<Q6d4Qd4i` (112 bytes), then a 64-byte rollback block.

| off | type | field |
|---|---|---|
| 0 | u64 | terminal_id |
| 8 | f64 | generated_mbit — *delta* |
| 16 | f64 | sent_to_switch_mbit — *delta* |
| 24 | f64 | received_mbit — *delta* |
| 32 | f64 | source_backlog_mbit — *gauge* |
| 40 | f64 | send_rate_sum_mbps — *gauge* |
| 48 | f64 | pause_time_ns — *delta* |
| 56 | u64 | pause_frames_received — *delta* |
| 64 | u64 | resume_frames_received — *delta* |
| 72 | u64 | pause_updates_received — *delta* |
| 80 | u64 | rate_updates_received — *delta* |
| 88 | f64 | end_time (virtual time of the sample) |
| 96 | i32 | attached_switch |
| 100 | i32 | active_flows — *gauge* |
| 104 | i32 | link_paused — *gauge* |
| 108 | i32 | reserved (always 0) |
| 112 | 4×f64 + 4×u64 | rollback block — ignore |

### Payload: fluid-flow WAN switch VT — `192 + 64P` bytes

`P` = max_ports, as for the GVT/RT switch payload. Recover it from the record's payload
size: `P = (sample_sz − 192) / 64` (must divide exactly and be ≥ 1). Example topology
(P = 4): **448 bytes**.

| off | type | field |
|---|---|---|
| 0 | u64 | switch_id |
| 8 | f64 | shared_buffer_capacity_mbit — static |
| 16 | f64 | shared_buffer_occupancy_mbit — *gauge* |
| 24 | f64 | buffered_residual_mbit — *gauge* |
| 32 | f64 | sent_mbit — *delta* |
| 40 | f64 | delivered_local_mbit — *delta* |
| 48 | f64 | dropped_mbit — *delta* |
| 56 | u64 | pause_frames_sent — *delta* |
| 64 | u64 | resume_frames_sent — *delta* |
| 72 | u64 | pause_updates_sent — *delta* |
| 80 | u64 | pause_frames_received — *delta* |
| 88 | u64 | resume_frames_received — *delta* |
| 96 | u64 | pause_updates_received — *delta* |
| 104 | f64 | end_time |
| 112 | i32 | num_ports |
| 116 | i32 | reserved (always 0) |
| 120 | — | per-port block, **identical layout to the GVT/RT payload's** (4 f64 arrays then 4 i32 arrays, each of length P, 48P bytes total) |
| 120+48P | 3×f64 + 6×u64 + 2×(f64 ×P) | rollback block — ignore |

### Why fluid-flow WAN sizes never collide with the dragonfly ones

Every dragonfly-dally and ping pong payload size is a multiple of 8 except the 44-byte ping
pong GVT payload. The fluid-flow WAN sizes are chosen so that the parametric switch sizes
(`112 + 48P`, `192 + 64P`) can never equal a dally router size (`8 + 32·radix`,
`72 + 32·radix`) for any integer radix — the two families differ by an odd multiple of 4 —
and the fixed terminal sizes (104, 176) avoid the dally terminal sizes at the default
1 rail / 1 QoS level. The two families never appear in the same simulation, but the
reference parser dispatches by size alone, so it checks for collisions and fails loudly if
an exotic `--num-rails`/`--num-qos`/`--radix` combination produces one (e.g. `--radix 3`
makes the dally router payload 104 bytes). Pass
`--model-family dragonfly|fluid-flow-wan` to drop the family the run did not use.

## Mapping LP ids to model entities

The first payload field of every record is the **model-relative id** (svr_id /
terminal_id / router_id / switch_id) — prefer it over the global `lpid`. For the
fluid-flow WAN models those ids index the topology YAML's declaration order: switches in
the order they appear under `topology.switches`, terminals numbered consecutively per
switch in that same order (switch A's terminals first, then B's, …). The switch payload's
`port_target_is_terminal` / `port_target_index` arrays give the peer of each port, so link
identity can be recovered from the records alone. For reference, the tutorial
config lays LPs out in 36 repetitions of [2 `nw-lp`, 2 terminals, 1 router], so global
`lpid = 5*rep + offset` with offsets 0–1 = servers, 2–3 = terminals, 4 = router
(model-net's internal LP ordering within a repetition follows the config file's group
listing order).

## Format stability

These payloads changed (fields appended, VT structs reworked) when the vis-oriented stats
were added to dragonfly-dally; any parser of the pre-2026-07 dally payloads needs
updating. Treat the layouts in this file as the source of truth and update it in the same
PR as any payload change.
