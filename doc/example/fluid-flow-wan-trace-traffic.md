# Fluid-flow WAN trace-traffic workload

`model-net-fluid-flow-wan-trace-traffic` reuses the same fluid-flow WAN network,
max-min rate feedback, shared-buffer, Ethernet PAUSE, statistical-egress, and
rollback implementation as the random-traffic workload. Only the source workload
is different.

The workload reads a CSV file configured with `traffic_trace_file`:

```csv
interval,flow_id,source_terminal,destination_terminal,offered_gbit
0,1001,0,5,20.0
1,1001,0,5,19.7
2,1001,0,5,20.2
```

The final column may be either `offered_mbit` or `offered_gbit`.

Each row adds the measured/offered volume to the source backlog for that
persistent `flow_id` at the beginning of that transmission interval. Reusing a
`flow_id` across rows preserves the flow's rate-feedback state across the whole
trace. The last row for a flow marks the workload complete; the final segment is
not advertised until the accumulated source backlog has drained.

Trace volume is offered demand, not forced delivery. Terminal access capacity,
rate feedback, and Ethernet PAUSE can therefore leave part of a trace interval's
offer in source backlog for later intervals.

`flow_id` values must be globally unique within the trace and must keep the same
source and destination terminals in every row. Terminal indices refer to the
terminal ordering induced by the topology YAML.

## ROSS instrumentation

Besides its own CSV logs, the model can emit per-LP time series through the ROSS
instrumentation layer (`--model-stats`, `--event-trace`). Terminal LPs report
generated/sent/received volume, source backlog, the sum of advertised send rates,
and PAUSE counters; switch LPs report shared-buffer occupancy, egress volume, drops,
PAUSE counters, and per-output-port capacity/queue/egress/pause-time arrays.

Copy the example inputs into a working directory first — the model resolves
`topology_yaml_file`, `traffic_trace_file`, and the CSV log paths relative to the
directory containing the config file (missing log directories are created
automatically). Run this from your build directory (e.g. `build/debug`):

```bash
mkdir -p ffw-stats-demo && cd ffw-stats-demo
cp ../doc/example/fluid-flow-wan-topology.yaml .
cp ../doc/example/fluid-flow-wan-trace-traffic.csv .
cp ../doc/example/fluid-flow-wan-trace-traffic.yaml .

mpirun -np 1 ../src/model-net-fluid-flow-wan-trace-traffic --sync=1 \
    --model-stats=3 --vt-interval=1e8 --vt-samp-end=1.7e10 \
    --stats-path=model-stats-out \
    -- fluid-flow-wan-trace-traffic.yaml
```

`--vt-samp-end` is **required** here. It defaults to `g_tw_ts_end`, which the model
sets to a year of virtual time, so without it the analysis LPs keep scheduling
sample events for simulated centuries after the traffic ends. The example config
runs `num_send_intervals + num_drain_intervals` = 16 intervals of 1 s, i.e. it
finishes at 1.6e10 ns; `--vt-interval=1e8` gives ten samples per fluid interval and
`--vt-samp-end=1.7e10` stops just past the end.

Virtual-time samples land in `model-stats-out/ross-stats-analysis-lps.bin`. Use
`--model-stats=1` (GVT) or `--model-stats=4` (everything) to also fill
`ross-stats-model.bin`; GVT mode needs a parallel run (`--sync=3`) because it samples
at intermediate GVTs. Decode either file with the reference parser:

```bash
python3 <codes-source-dir>/scripts/parse-ross-model-stats.py \
    model-stats-out/ross-stats-analysis-lps.bin --csv-prefix ffw
# writes ffw-ffw-terminal.csv and ffw-ffw-switch.csv
```

All data volumes in the binary payloads are raw Mbit, regardless of whether the
topology YAML used Mb or Gb units. The exact byte layouts are in
[../model-stats-binary-format.md](../model-stats-binary-format.md).

## PAUSE demo variant

With the stock 64 Gb deep buffers this trace never crosses the PAUSE high
watermark — max-min rate feedback caps switch B's standing backlog near 1.7 Gb,
so rate feedback is the only backpressure signal you will see. To also exercise
Ethernet PAUSE/RESUME, use `fluid-flow-wan-trace-traffic-pause.yaml`, which is
identical except that it points at `fluid-flow-wan-topology-pause.yaml` (2 Gb
buffers, PAUSE watermarks at 1.6/1 Gb). The same trace then drives switch B
across the watermark: B PAUSEs its largest ingress contributor (the A->B link)
and the PAUSE/RESUME frames, per-port pause time, and paused-port samples all
appear in the CSV logs and in the instrumentation payloads.
