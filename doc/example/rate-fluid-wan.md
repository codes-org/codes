# Rate-based fluid WAN model

`model-net-rate-fluid-wan` is an event-driven fluid WAN workload.  It does not
use a forwarding interval.  Data and control propagation both use the physical
`link_latency` configured in `RATE_FLUID_WAN`.

## Event workflow

- `FLOW_START`: source-terminal workload event.  Creates a finite source
  backlog and sends `FLOW_REGISTER` toward the attached switch.
- `FLOW_REGISTER`: travels downstream one link at a time and creates per-flow
  state along the path.  The final switch can immediately determine the first
  end-to-end admissible rate.
- `RATE_FEEDBACK`: travels upstream and carries the rate that the downstream
  path currently permits.  A flow start, flow completion, new contention, or a
  changed downstream constraint can cause new feedback.
- `DATA_RATE_UPDATE`: travels downstream and carries the *actual* data rate
  emitted by the upstream terminal/switch.  This is intentionally distinct
  from `RATE_FEEDBACK`: control information must first reach the source before
  the physical incoming data rate changes downstream.
- `BUFFER_THRESHOLD`: switch-wide shared-buffer event used only for predicted
  HIGH and LOW watermark crossings. Queue occupancy is integrated analytically
  between state-changing events. Per-flow queue drain timing is handled by an
  internal `QUEUE_DRAIN` event and is not a buffer-threshold condition.
- `PAUSE_UPDATE`: flow-selective PAUSE/RESUME propagated upstream using the same
  link latency. At a HIGH watermark, the switch chooses the largest current
  incoming-rate contributors until the present buffer-growth rate is arrested;
  at LOW, it resumes the flows that it paused. Local egress scheduling always
  enforces link capacity while PAUSE protects the finite shared buffer.
- `FLOW_COMPLETE`: predicted source completion.  The source emits a zero
  `DATA_RATE_UPDATE` and propagates completion downstream.  Buffered data is
  still allowed to drain before the destination is marked complete.

When `debug: 1`, each switch prints its shared-buffer occupancy whenever the
analytically integrated occupancy changes. These debug messages are observational; HIGH/LOW threshold
timing remains analytically predicted from the current aggregate queue slope.

Every switch event first advances all queue state from the preceding event time
to the current ROSS timestamp.  For a flow with constant rates over `dt`,

```
queue += (incoming_rate - outgoing_rate) * dt
```

with the rate-to-data conversion determined from the configured rate/data unit
pair.  The same approach is used by source remaining-data and destination
received-data accounting.

## Units

Rates accept only `Mbps` and `Gbps`.

Flow sizes accept `MB` and `GB`.  `switch_buffer` accepts `MB`/`GB`; legacy
`Mb`/`Gb` buffer spellings are also accepted and converted to bytes.

The model preserves homogeneous units:

- all `Gbps` -> rates are simulated/reported in `Gbps`;
- all `Mbps` -> rates are simulated/reported in `Mbps`;
- mixed `Gbps`/`Mbps` -> rates are normalized to `Mbps`;
- all `GB` -> data are simulated/reported in `GB`;
- all `MB` -> data are simulated/reported in `MB`;
- mixed `GB`/`MB` -> data are normalized to `MB`.

Flow start times accept `s` and `ms` (the latency parser also accepts `us` and
`ns`).  If all flow start times use seconds, output times use seconds.  If
millisecond and second start specifications are mixed, output uses
milliseconds.

## Files

The topology YAML follows the earlier fluid-flow-WAN layout:

```yaml
topology:
  switches:
    S0:
      terminals: 2
      terminal_bandwidth: "100 Gbps"
      switch_buffer: "8 GB"
      connections:
        S1: "25 Gbps"
```

The flow CSV is one row per finite transfer:

```csv
start_time,flow_id,source_terminal,destination_terminal,size
0 s,1,0,5,12 GB
1 s,2,2,5,4 GB
```

The preferred current-CODES configuration is YAML.  The LP layout uses the
`groups` topology escape hatch and model-specific knobs live under `sections`:

```yaml
schema_version: 1

topology:
  format: groups
  params:
    message_size: 65536
  groups:
    RATE_FLUID_WAN_GRP:
      repetitions: 1
      lps:
        rate-fluid-wan-switch-lp: 3
        rate-fluid-wan-terminal-lp: 6

simulation:
  end_time: "30s"
  pe_mem_factor: 1024

sections:
  rate_fluid_wan:
    topology_file: rate-fluid-wan-topology.yaml
    flow_file: rate-fluid-wan-flows.csv
    link_latency: "1 ms"
    pause_enabled: 1
    pause_high_watermark_fraction: 0.80
    pause_low_watermark_fraction: 0.50
    debug: 1
    flow_summary_file: rate-fluid-flow-summary.csv
```

A legacy `.conf` equivalent is also included as `rate-fluid-wan.conf`; the
executable uses the normal CODES configuration API and accepts either form.

With `debug="1"`, every event is printed, for example:

```text
FLOW START(F1) at terminal S0.0 at 0.000000 s
FLOW REGISTER(F1) at switch S0 at 0.001000 s
RATE FEEDBACK(F1) at switch S0 at 0.005000 s rate=25.000000 Gbps
DATA RATE UPDATE(F1) at switch S0 at 0.007000 s rate=25.000000 Gbps
```

## Build and run

After configuring and rebuilding CODES, run from the build directory:

```bash
mpirun -np 1 ./src/model-net-rate-fluid-wan --sync=1 -- \
  doc/example/rate-fluid-wan.yaml
```

The generated example configuration, topology, and flow CSV are placed in
`build/doc/example/`. The flow summary is written to
`build/rate-fluid-flow-summary.csv` when the command above is launched from
the build directory.

The example uses a generous `message_size` because the event message
contains rollback metadata for continuous queue/rate state.  The executable
checks `sizeof(rate_msg)` against the configured ROSS message size at startup.
