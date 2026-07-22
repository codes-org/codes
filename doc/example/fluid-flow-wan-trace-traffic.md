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
