# Running with a YAML config

CODES accepts a YAML (or JSON) configuration anywhere it accepts a legacy
`.conf` today. Both formats are supported side by side — the format is chosen per
file, by extension:

- `.yaml`, `.yml`, `.json` → the YAML front-end
- anything else → the legacy `.conf` text parser

Point an existing executable at a YAML file exactly as you would a `.conf` — the
config path is the trailing positional argument, after ROSS's `--`:

```bash
./model-net-synthetic --sync=1 --num_messages=1 -- my-network.yaml
```

Nothing else changes: the YAML front-end compiles the friendly format down to the
same internal configuration the `.conf` parser produces, so every model reads it
unchanged. A YAML config and its `.conf` twin drive a model to identical results.

The YAML front-end is always available — RapidYAML is vendored in-tree
(`thirdparty/rapidyaml/`) and built unconditionally, so there is nothing to
enable and no configure-time option to set.

## Shape of a config

A config has a few top-level blocks:

```yaml
schema_version: 1   # required; this build understands version 1
components:          # named component configs referenced by the topology
topology:            # a flat network, or a parametric fabric
```

A **component** pairs a model with its parameters and is referenced by name from
the topology. `schema_version` is required and must be a version this build
knows; unknown top-level keys, unknown topology keys, and blocks a component or
fabric doesn't consume are errors, not silent drops.

## Flat networks

The flat all-to-all network models — `simplenet` and `simplep2p` — are described
as a single component plus a node count. The component bundles the workload model
with the NIC model it runs over:

```yaml
schema_version: 1

components:
  compute_node:
    model: nw-lp            # the workload
    network: simplenet      # the NIC model the workload runs over
    packet_size: 512
    message_size: 464
    modelnet_scheduler: fcfs
    net_startup_ns: 1.5
    net_bw_mbps: 20000

topology:
  format: flat
  component: compute_node
  nodes: 16
```

`nodes` becomes the number of compute-node slots. `simplep2p` takes its per-link
latency/bandwidth from the existing matrix files, referenced by path from the
component (`net_latency_ns_file`, `net_bw_mbps_file`); the flat form supplies
only the node count. (A node/edge graph form — per-node overrides, per-edge link
rates — is a separate representation that lands with the model that consumes it.)

## Parametric fabric (HPC networks)

Regular HPC fabrics are not drawn node by node — their connectivity follows from
a handful of shape parameters. They use `format: parametric` and a `fabric`
block instead of a node count:

```yaml
schema_version: 1

components:
  compute_host:
    model: nw-lp

topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: 8         # routers per group; the rest of the layout follows
    links:
      local:  { bandwidth: 5.25, vc_size: 4096 }
      global: { bandwidth: 4.7,  vc_size: 8192 }
      cn:     { bandwidth: 5.25, vc_size: 4096 }
    routing:
      algorithm: adaptive
    packet_size: 512
    chunk_size: 32
    num_vcs: 1
    modelnet_scheduler: fcfs
    message_size: 512
  hosts:
    component: compute_host    # the workload on every terminal
```

The compiler derives the group, repetition, and per-router counts from the
`shape` (the same math the model does internally), maps the per-link-class
`links` and `routing` onto the model's parameters, and runs the fabric's
connectivity generation exactly as today.

Supported fabric `model`s are `dragonfly`, `dragonfly-dally`, and `fattree`.
`dragonfly-dally` is *file-enumerated*: its wiring comes from binary connection
files produced by the existing generator scripts, referenced by path so the
model reads them unchanged:

```yaml
    connections:
      intra: conf/dragonfly-dally/dfdally-72-intra
      inter: conf/dragonfly-dally/dfdally-72-inter
```

For a file-enumerated fabric the `shape` counts are inputs that must stay
consistent with the connection files.

## Worked examples in the tree

Each of these YAML files is a twin of the `.conf` beside it, checked in CI to
produce identical results:

- `tests/conf/modelnet-test-simplenet.yaml`
- `tests/conf/modelnet-test-simplep2p.yaml`
- `src/network-workloads/conf/modelnet-synthetic-dragonfly.yaml`
- `src/network-workloads/conf/modelnet-synthetic-fattree.yaml`
- `tests/conf/dragonfly-dally/dfdally-72.yaml.in` (dragonfly-dally)
