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

- **`schema_version`** is required, an integer, and must be a version this build
  knows (currently `1`). A newer, unknown version is a hard error rather than a
  best-effort guess.
- **`components`** is a map of name → component. A **component** pairs a model
  (`model:`) with its parameters; a flat-network component also names the NIC
  model it runs over (`network:`). Components are referenced by name from the
  topology.
- **`topology`** selects the network. `format: flat` is an all-to-all point-to-
  point network described by a component and a node count; `format: parametric`
  is an HPC fabric described by shape parameters.

Validation is strict: unknown top-level keys, unknown topology keys, a block a
component or fabric doesn't consume, and (for flat topologies) an unexpected
`edges`/graph block are all **errors, not silent drops**. A malformed value
(e.g. a non-integer node count) is likewise rejected with a diagnostic.

### How it compiles

The compiler walks the friendly form and emits the `LPGROUPS` (LP layout) and
`PARAMS` (model knobs) the models already read:

- a flat network becomes one repetition per node — a `[workload, NIC]` LP pair;
- a parametric fabric derives its repetition / per-router / per-group counts from
  the `shape` (the same arithmetic the model does internally) and lays out the
  `[workload, terminal, router]` LPs;
- any scalar key the compiler doesn't special-case is **passed through verbatim**
  to `PARAMS`, so advanced model knobs need no compiler change.

---

# Flat networks

The flat point-to-point network models — `simplenet`, `simplep2p`, and `loggp` —
are described as a single **component** plus a **node count**. The component
bundles the workload model (`model:`) with the NIC model it runs over
(`network:`), and carries the NIC's parameters directly. The topology supplies
only how many peer nodes to instantiate:

```yaml
topology:
  format: flat
  component: <component name>
  nodes: <count>
```

`nodes` becomes the number of compute-node slots (repetitions). All flat models
are uniform all-to-all — there is no per-node or per-edge form in a flat config;
a model whose links vary per pair reads them from its own file (see `simplep2p`
and `loggp` below).

## simplenet

A uniform all-to-all network with a single startup latency and bandwidth. No
external file — the rates sit directly on the component.

Component keys: `net_startup_ns`, `net_bw_mbps`, plus the usual `packet_size`,
`message_size`, `modelnet_scheduler`.

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

## simplep2p

Point-to-point with a per-pair latency/bandwidth matrix. The matrices come from
the existing files, referenced by path on the component (`net_latency_ns_file`,
`net_bw_mbps_file`); the model resolves them relative to the config file's
directory, so a bare name sits next to the config.

```yaml
schema_version: 1

components:
  compute_node:
    model: nw-lp
    network: simplep2p
    message_size: 464
    packet_size: 1024
    modelnet_scheduler: fcfs
    net_latency_ns_file: modelnet-test-latency.conf
    net_bw_mbps_file: modelnet-test-bw.conf

topology:
  format: flat
  component: compute_node
  nodes: 3
```

## loggp

The LogGP point-to-point model. Its parameter table (`G`, latency, etc.) comes
from a network-config file referenced by path (`net_config_file`), resolved
relative to the config file's directory like simplep2p's matrices.

```yaml
schema_version: 1

components:
  compute_node:
    model: nw-lp
    network: loggp
    message_size: 464
    modelnet_scheduler: fcfs-full
    net_config_file: ng-mpi-tukey.dat

topology:
  format: flat
  component: compute_node
  nodes: 16
```

---

# Parametric fabrics (HPC networks)

HPC fabrics are not drawn node by node — their connectivity follows from a
handful of shape parameters. They use `format: parametric` and a `fabric` block,
with a `hosts.component` naming the per-terminal workload:

```yaml
topology:
  format: parametric
  fabric:
    model: <fabric model>
    shape:      { ... }    # size knobs; the compiler derives LP counts from these
    links:      { ... }    # optional per-link-class bandwidth / vc_size sugar
    routing:    { ... }    # routing.algorithm -> the model's "routing" param
    connections:{ ... }    # file-enumerated fabrics only: intra/inter wiring files
    # any other scalar key here passes straight through to PARAMS
  hosts:
    component: <component name>   # the workload on every terminal
```

Building blocks shared by all fabrics:

- **`shape`** — the size knobs specific to each model (below). The compiler runs
  the same shape→counts arithmetic the model does, so you set the small,
  meaningful numbers and the repetition/group counts follow.
- **`links`** — optional sugar for the `local` / `global` / `cn` link-class
  pattern: `local: { bandwidth: 5.25, vc_size: 4096 }` expands to
  `local_bandwidth=5.25` and `local_vc_size=4096`. Models that name their link
  parameters flat (e.g. `torus`'s `link_bandwidth`) just set those keys directly
  instead — anything not recognized is passed through verbatim.
- **`routing`** — `routing.algorithm: minimal` becomes the model's `routing`
  param; any other `routing.*` key passes through under its own name.
- **`connections`** — file-enumerated fabrics only: `intra`/`inter` name the
  binary wiring files (`intra-group-connections` / `inter-group-connections`).
- **pass-through** — any remaining scalar key on the `fabric` (e.g.
  `packet_size`, `chunk_size`, `message_size`, `modelnet_scheduler`) lands in
  `PARAMS` unchanged. A list value (e.g. `slimfly`'s generator sets) is emitted
  as a multi-value key.

Supported fabric `model`s split into **internally-generated** (the compiler emits
only shape params and the model generates its wiring) and **file-enumerated** (the
wiring is read from binary connection files produced by the generator scripts).

## Internally-generated fabrics

### dragonfly

Regular (Kim–Dally) dragonfly. The whole layout follows from one number,
`num_routers` (routers per group): the model uses `num_cn = num_routers / 2`
terminals per router and `num_groups = num_routers * num_cn + 1`.

Shape: `num_routers`.

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
    component: compute_host
```

### torus

An n-dimensional torus. It folds routing into the terminal node, so there is
**no separate router LP**; the node count is the product of the per-dimension
lengths. `dim_length` is a comma-separated string the model parses itself, so it
is written as a quoted scalar (not a YAML list). Bandwidth is a single flat
`link_bandwidth`, not a per-link-class block.

Shape: `n_dims`, `dim_length` (repetitions = product of `dim_length`).

```yaml
schema_version: 1

components:
  compute_host:
    model: nw-lp

topology:
  format: parametric
  fabric:
    model: torus
    shape:
      n_dims: 3
      dim_length: "4,2,2"      # node count = 4*2*2 = 16
    packet_size: 512
    chunk_size: 256
    message_size: 464
    modelnet_scheduler: fcfs
    link_bandwidth: 2.0
    buffer_size: 4096
    num_vc: 1
  hosts:
    component: compute_host
```

### slimfly

An MMS (McKay–Miller–Širáň) slimfly built on `num_routers`: the two Cayley
subgraphs give `2 * num_routers^2` routers total, each hosting `num_terminals`
terminals. The generator sets are list-valued, written as YAML sequences and
emitted as multi-value params (`generator_set_X=("1","4")`).

Shape: `num_routers`, `num_terminals` (repetitions = `2 * num_routers^2`).

```yaml
schema_version: 1

components:
  compute_host:
    model: nw-lp

topology:
  format: parametric
  fabric:
    model: slimfly
    shape:
      num_routers: 5           # -> 2 * 5^2 = 50 routers
      num_terminals: 3
    links:
      local:  { bandwidth: 9.0, vc_size: 25600 }
      global: { bandwidth: 9.0, vc_size: 25600 }
      cn:     { bandwidth: 9.0, vc_size: 25600 }
    routing:
      algorithm: minimal
    generator_set_X: [1, 4]
    generator_set_X_prime: [2, 3]
    packet_size: 512
    chunk_size: 256
    message_size: 464
    modelnet_scheduler: fcfs
    num_vcs: 4
    global_channels: 5
    local_channels: 2
    link_delay: 0
  hosts:
    component: compute_host
```

### express-mesh

An n-dimensional express mesh with a separate router LP. The router count is the
product of the per-dimension lengths, and each router hosts `num_cn` terminals.
Like torus it uses flat bandwidth keys (`link_bandwidth`, `cn_bandwidth`, ...)
rather than a `links` block, and `dim_length` is a quoted scalar.

Shape: `n_dims`, `dim_length` (router count = product of `dim_length`), `num_cn`.

```yaml
schema_version: 1

components:
  compute_host:
    model: nw-lp

topology:
  format: parametric
  fabric:
    model: express-mesh
    shape:
      n_dims: 3
      dim_length: "4,4,4"      # router count = 4*4*4 = 64
      num_cn: 3                # terminals per router
    routing:
      algorithm: static
    message_size: 512
    packet_size: 4096
    chunk_size: 4096
    modelnet_scheduler: round-robin
    gap: 1
    num_vcs: 1
    link_bandwidth: 12.5
    cn_bandwidth: 12.5
    vc_size: 65536
    cn_vc_size: 65536
    soft_delay: 0
    router_delay: 90
  hosts:
    component: compute_host
```

### fattree

A multi-level fat-tree. One repetition per edge switch, each hosting
`switch_radix / 2` terminals, with one switch LP per level. The fattree switch is
not a separate model-net method, so it does not appear in `modelnet_order`.
Bandwidth is set with flat keys (`link_bandwidth`, `cn_bandwidth`).

Shape: `num_levels`, `switch_count`, `switch_radix`
(repetitions = `switch_count`, terminals per switch = `switch_radix / 2`).

```yaml
schema_version: 1

components:
  compute_host:
    model: nw-lp

topology:
  format: parametric
  fabric:
    model: fattree
    shape:
      num_levels: 3
      switch_count: 32
      switch_radix: 8
    routing:
      algorithm: adaptive
    ft_type: 0
    packet_size: 512
    message_size: 512
    chunk_size: 512
    modelnet_scheduler: fcfs
    router_delay: 90
    terminal_radix: 1
    soft_delay: 1000
    vc_size: 65536
    cn_vc_size: 65536
    link_bandwidth: 12.5
    cn_bandwidth: 12.5
    rail_routing: adaptive
  hosts:
    component: compute_host
```

## File-enumerated fabrics

These fabrics read their wiring from binary connection files produced by the
existing generator scripts. Add a `connections` block naming the `intra`/`inter`
files by path; the model reads them unchanged. For a file-enumerated fabric the
`shape` counts are **inputs that must stay consistent with the connection
files** — they are not free to choose independently of the wiring.

```yaml
    connections:
      intra: /path/to/<name>-intra
      inter: /path/to/<name>-inter
```

Paths are resolved by the model as given (relative to the run directory), so
tests use absolute paths (`@CMAKE_SOURCE_DIR@` substituted by CMake) to stay
independent of where the run happens; the `.yaml.in` twins in the tree show the
pattern.

### dragonfly-dally

Total routers = `num_groups * num_planes * num_routers` (with `num_planes`
defaulting to 1); each router hosts `num_cns_per_router` terminals.

Shape: `num_routers`, `num_groups`, `num_cns_per_router`, `num_global_channels`,
optional `num_planes`.

```yaml
schema_version: 1

components:
  compute_host:
    model: nw-lp

topology:
  format: parametric
  fabric:
    model: dragonfly-dally
    shape:
      num_routers: 4
      num_groups: 9
      num_cns_per_router: 2
      num_global_channels: 2
    links:
      local:  { bandwidth: 2.0, vc_size: 16384 }
      global: { bandwidth: 2.0, vc_size: 16384 }
      cn:     { bandwidth: 2.0, vc_size: 32768 }
    routing:
      algorithm: minimal
    connections:
      intra: /abs/path/conf/dragonfly-dally/dfdally-72-intra
      inter: /abs/path/conf/dragonfly-dally/dfdally-72-inter
    packet_size: 4096
    chunk_size: 4096
    message_size: 736
    modelnet_scheduler: fcfs
    minimal-bias: 1
    df-dally-vc: 1
  hosts:
    component: compute_host
```

### dragonfly-plus

One repetition per group. Each group's routers split into a spine and a leaf
level (`num_router_spine + num_router_leaf` router LPs), and only the leaf
routers host terminals (`num_cns_per_router` each), so terminals per group =
`num_router_leaf * num_cns_per_router`.

Shape: `num_router_spine`, `num_router_leaf`, `num_groups`, `num_cns_per_router`.

```yaml
schema_version: 1

components:
  compute_host:
    model: nw-lp

topology:
  format: parametric
  fabric:
    model: dragonfly-plus
    shape:
      num_router_spine: 4
      num_router_leaf: 4
      num_groups: 5
      num_cns_per_router: 4
    links:
      local:  { bandwidth: 5.25, vc_size: 8192 }
      global: { bandwidth: 1.5,  vc_size: 16384 }
      cn:     { bandwidth: 8.0,  vc_size: 8192 }
    routing:
      algorithm: prog-adaptive
    connections:
      intra: /abs/path/conf/dragonfly-plus/dfp-test-intra
      inter: /abs/path/conf/dragonfly-plus/dfp-test-inter
    packet_size: 1024
    chunk_size: 1024
    message_size: 608
    modelnet_scheduler: fcfs
    num_level_chans: 1
    num_global_connections: 4
    route_scoring_metric: delta
  hosts:
    component: compute_host
```

### dragonfly-custom

Each group is a `num_router_rows × num_router_cols` mesh of routers, so total
routers = `num_groups * num_router_rows * num_router_cols`; each router hosts
`num_cns_per_router` terminals.

Shape: `num_router_rows`, `num_router_cols`, `num_groups`, `num_cns_per_router`,
`num_global_channels`.

```yaml
schema_version: 1

components:
  compute_host:
    model: nw-lp

topology:
  format: parametric
  fabric:
    model: dragonfly-custom
    shape:
      num_router_rows: 6
      num_router_cols: 16
      num_groups: 8
      num_cns_per_router: 4
      num_global_channels: 4
    links:
      local:  { bandwidth: 12.5, vc_size: 65536 }
      global: { bandwidth: 12.5, vc_size: 65536 }
      cn:     { bandwidth: 12.5, vc_size: 65536 }
    routing:
      algorithm: adaptive
    connections:
      intra: /abs/path/scripts/dragonfly-custom/example/intra-theta-8group
      inter: /abs/path/scripts/dragonfly-custom/example/inter-theta-8group
    packet_size: 4096
    message_size: 656
    chunk_size: 4096
    modelnet_scheduler: fcfs
    router_delay: 90
  hosts:
    component: compute_host
```

---

# Explicit LP groups: `format: groups`

The flat and parametric forms above each describe a single network. Some configs
aren't a single network at all — a storage cluster, a mapping test, several
partitions side by side — and there is nothing for the compiler to derive. For
those, lay the LP groups out directly with `format: groups`:

```yaml
schema_version: 1

topology:
  format: groups
  params:                      # -> PARAMS, written out verbatim
    message_size: 512
  groups:                      # -> LPGROUPS, one entry per group
    TRITON_GRP:
      repetitions: 1
      lps:                     # LP type -> count within each repetition
        nw-lp: 1
        lsm: 1
```

Each group names its `repetitions` and, under `lps`, the LP types with their
per-repetition counts — a direct, validated transcription of a `.conf`
`LPGROUPS`. There is no network to derive from, so `modelnet_order` and any other
knobs come from whatever you put in `params`. Group and LP-type names are
free-form (they match what each model registers).

**Annotations.** `codes_mapping` lets the same LP type appear more than once in a
group under different annotations. Write the annotation on the LP-type key as
`type@annotation`:

```yaml
    lps:
      a: 1
      a@foo: 1     # LP type "a", annotation "foo" -- a distinct entry from "a"
```

Combine this with `sections:` (below) for a model that also reads its own config
section — e.g. a storage model's `lsm` or `resource` block.
`tests/conf/lsm-test.yaml` and `tests/conf/buffer_test.yaml` are full twins doing
exactly that.

---

# Config a model reads directly: `sections:`

Not every subsystem's config is topology the compiler derives. Many read their
own named section straight from the config — the storage model reads `resource`,
the surrogate/director stack reads `NETWORK_SURROGATE`, `APPLICATION_SURROGATE`,
and `DIRECTOR`, and so on. Those keys aren't transformed, just read, so the YAML
front-end carries them through **verbatim** under a top-level `sections:` block:

```yaml
schema_version: 1
topology: { ... }          # friendly, derived, strictly validated

sections:                  # emitted verbatim as top-level config sections
  resource:
    available: 8192
  network_surrogate:
    enable: 1
    fixed_switch_timestamps: [25.0e6, 400.0e6]   # a list -> a multi-value key
    torch_jit:                                   # a nested block -> a subsection
      mode: single-static-model-for-all-terminals
```

Inside a section, a scalar becomes a single-value key, a **list** becomes a
multi-value key (`("a","b")`), and a **nested map** becomes a subsection. There
is no fixed schema — a section can carry whatever keys the model reads.

**Section names are case-insensitive.** The all-caps convention (`PARAMS`,
`DIRECTOR`, `NETWORK_SURROGATE`) is a historical carryover; write `resource` or
`network_surrogate` (or any case) and the model finds it. **Keys inside a section
stay case-sensitive** — a model reads them by exact name, so `available` and
`Available` are different keys. `LPGROUPS` and `PARAMS` are reserved: the compiler
emits them from the topology, so they can't appear under `sections:` (put model
parameters on the component or fabric instead).

## Adding a config section

When a new feature needs configuration, decide which of two tiers it belongs to.
The test is one question: **does the compiler need to derive, rename, or
cross-check anything?**

**Tier 1 — the model reads the keys as-is (most sections).** No compiler change:

1. read your keys in the model with `configuration_get_value(&config,
   "my_section", ...)`;
2. put them under `sections:` in the YAML;
3. document the section and its keys here.

Optionally, register an **open schema** to enforce required keys while still
allowing anything else through — handy for a section that's still in flux, where
you know a couple of keys are mandatory but the rest are unsettled. Add a row to
`section_schemas[]` in `src/modelconfig/config_compiler.cxx`:

```cpp
const char* const my_required[] = {"must_have", nullptr};
const section_schema section_schemas[] = {
    {"resource", resource_required},
    {"my_section", my_required},   // required keys checked; unlisted keys pass through
};
```

A missing required key becomes a `config_error` up front (a clearer, earlier
message than a mid-run abort). A section with no registered schema passes through
with no validation at all.

**Tier 2 — the compiler derives or transforms the config (topology, and later
jobs).** This earns a first-class module: a parser, a compiler that emits the
derived sections, a registry entry, and a `.conf`-equivalence test — the pattern
the `fabric_models[]` table and `compile_fabric()` already follow. Reach for this
only when the compiler is doing real work (shape→counts, friendly→internal names,
cross-section consistency); otherwise Tier 1 is the answer.

The compiler core is ROSS-free and unit-tested in isolation
(`tests/codes-config-compiler-test.cxx`); add cases there for whatever a new
section or tier introduces.

---

# Reusing config across files: `include:`

A config can pull in other files with a top-level `include:` — a filename, or a
list of them, resolved relative to the including file's directory:

```yaml
schema_version: 1
include: [ common.yaml, sections/storage.yaml ]
topology: { ... }        # this file's own keys
```

Included files are the **base**; the including file **overrides** them. So you
can factor out a shared piece and vary the rest — define a network once and run
it under different configs, or (as in `lsm-test-include.yaml`) share a model
section across layouts. Merge rules:

- `components:` and `sections:` merge **by name** — an included set plus your own
  additions, with a local entry of the same name winning.
- `topology:` and `schema_version:` are singular — a local one replaces the
  included one.
- Multiple includes apply in list order; the local file is applied last.

Includes are resolved when the config is loaded, *before* compilation, so the
referenced files must be readable by every rank. An included file may not itself
use `include:` (one level deep, for now). The pure compiler core does no file
I/O — the loader reads the fragments and hands them in — so `include:` is a
loader feature a model never sees.

---

## Worked examples in the tree

Each of these YAML files is a twin of the `.conf` beside it, checked in CI to
produce identical results — the authoritative, runnable reference for each model:

| Model | Example |
|-------|---------|
| simplenet       | `tests/conf/modelnet-test-simplenet.yaml` |
| simplep2p       | `tests/conf/modelnet-test-simplep2p.yaml` |
| loggp           | `tests/conf/modelnet-test-loggp.yaml` |
| dragonfly       | `src/network-workloads/conf/modelnet-synthetic-dragonfly.yaml` |
| torus           | `tests/conf/modelnet-test-torus.yaml` |
| slimfly         | `tests/conf/modelnet-test-slimfly.yaml` |
| express-mesh    | `tests/conf/modelnet-test-em.yaml` |
| fattree         | `src/network-workloads/conf/modelnet-synthetic-fattree.yaml` |
| dragonfly-dally | `tests/conf/dragonfly-dally/dfdally-72.yaml.in` |
| dragonfly-plus  | `tests/conf/dragonfly-plus/dfp-test.yaml.in` |
| dragonfly-custom| `tests/conf/dragonfly-custom/dfcustom-8group.yaml.in` |
| explicit groups (storage/lsm) | `tests/conf/lsm-test.yaml` |
| explicit groups (resource)    | `tests/conf/buffer_test.yaml` |
| `include:` composition        | `tests/conf/lsm-test-include.yaml` (+ `lsm-workload.yaml`) |

The `.yaml.in` files are CMake templates (the `@CMAKE_SOURCE_DIR@` in their
connection paths is substituted at configure time); the plain `.yaml` files are
ready to run as-is.
