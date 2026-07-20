# Fluid-Flow WAN Statistical Hybrid Workflow

The fluid-flow WAN model keeps path-rate feedback inside the CODES model. Every
`SWITCH_RATE_EVAL` performs a full max-min allocation across the active flows on
one output port. A changed downstream cap schedules another port-wide
`SWITCH_RATE_EVAL`, so released capacity is redistributed and all affected rates
are re-advertised upstream.

The selectable hybrid component controls only switch egress volume:

```yaml
egress_model: pdes          # use the complete physical link budget
egress_model: statistical   # query the ZeroMQ per-switch statistical model
```

In statistical mode, every active `SWITCH_EGRESS_EARLY` and
`SWITCH_EGRESS_LATE` event queries the corresponding per-switch statistical
model. The request contains the physical capacity remaining in the interval,
the data eligible in that phase, and the current PAUSE state. The statistical
model returns:

```text
predicted_egress_mbit =
    output_paused ? 0 : min(remaining_capacity_mbit, eligible_data_mbit)
```

`SWITCH_EGRESS_EARLY` considers only previously buffered residual flowlets.
`SWITCH_EGRESS_LATE` considers only current-interval staged arrivals and uses
capacity left by the early phase. The switch LP then distributes the returned
phase volume among eligible flowlets using deterministic max-min service.

The statistical model requires no record collection or training. A separate
server-side analytical model instance is selected by switch ID. In the current
validation backend, its result must exactly match the local pure-PDES phase
calculation. This makes committed simulation behavior identical while exposing
the ZeroMQ request/response overhead. Inference is read-only and deterministic,
so optimistic re-execution receives the same result.

## Pure PDES

Set:

```yaml
egress_model: pdes
```

Then run from the build directory:

```bash
cd ~/codes/build
mpirun -np 1 \
  ./src/model-net-fluid-flow-wan \
  --sync=1 -- \
  doc/example/fluid-flow-wan.yaml
```

The ZeroMQ server is not required.

## Statistical hybrid

Start one server and leave it running:

```bash
conda activate Director
cd ~/codes
python3 -u src/surrogate/zmqml/zmqmlserver.py
```

Optionally inspect the statistical registry:

```bash
cd ~/codes/build
python3 ~/codes/src/surrogate/zmqml/zmqmlctl.py \
  --family fluid-flow-wan \
  --backend statistical \
  status
```

Set:

```yaml
egress_model: statistical
```

Then run:

```bash
cd ~/codes/build
mpirun -np 1 \
  ./src/model-net-fluid-flow-wan \
  --sync=1 -- \
  doc/example/fluid-flow-wan.yaml
```

The current statistical backend is an exact analytical reference model for
validating per-phase ZeroMQ integration. Future foundational switch models can
replace this backend while preserving the same request location and physical
bound checks in the CODES switch LP.
