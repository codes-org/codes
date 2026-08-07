# CODES Topology & Config Contract

**Status:** Draft v0

This document defines the **file format that flows between NetMaestro's topology
editor (or a hand-author) and the CODES configuration front-end**. It is the
interface contract: NetMaestro's editor *exports* it, hand-authors *write* it, and
the CODES "config compiler" (see [refactor-roadmap.md](refactor-roadmap.md) §7)
*consumes* it. It is versioned (`schema_version`); breaking changes bump the version.

It replaces the legacy `.conf` (`LPGROUPS` + `PARAMS` + `@annotation`) **and** eventually
other files, like the
separate latency-bandwidth matrix files, with a single
format read by one parser.

---

## 1. Principles

1. **Topology and parameters are separate concerns.** The topology graph answers
   *what connects to what* and *what each node is*. It does **not** carry the bulk
   of model parameters — those live in **custom components** referenced by name. A
   node tags a component; it does not repeat the component's parameters. Exception: HPC
   specific fabrics will be handled differently
2. **One parser.** Topology is expressed in [Cytoscape.js][cyto] element form, which
   is JSON. Because JSON is a subset of YAML, the same parser (RapidYAML) reads both
   an editor-exported `.json` and a hand-written `.yaml`.
3. **CODES reads an allowlist.** Of each element's `data`, CODES consumes only the
   documented keys below. Everything editor/visual — `position`, `classes`, `style`,
   `selected`, `locked`, `scratch`, … — is ignored, so the editor may add visual
   state freely without affecting the simulation.
4. **Implementation details never appear.** Anything CODES can derive (the ROSS
   event size, network ordering, repetition counts) is *computed by the compiler*,
   not written by the user. See §9.
5. **Units should be explicit.** Physical quantities can carry their unit (`"100Gbps"`,
   `"5ms"`); the compiler converts to whatever each model uses internally. If no
   unit is specified, then a default will be assumed.

[cyto]: https://js.cytoscape.org/#notation/elements-json

---

## 2. Vocabulary

| Domain term | NetMaestro DB model | In this file | Meaning |
|---|---|---|---|
| component model | `ComponentModel` | `model:` | a CODES simulation model from the catalog (e.g. `nw-lp`, `simplep2p`) |
| component type | `ComponentType` | `type:` | `host` \| `router` \| `switch`; usually inferred from the model |
| custom component | `ComponentConfig` | an entry under `components:` | a model paired with configured parameters — **what a node references** |
| node | `TopologyNode` | a Cytoscape node | a placed component (graph vertex) |
| link | `TopologyLink` | a Cytoscape edge | a graph edge (a physical link or an attachment) |
| group | `TopologyGroup` | `data.parent` / a `groups:` entry | optional grouping of nodes (e.g. a site) — the multi-network seam |
| topology | `Topology` | the `topology:` block | the saved layout (nodes + links + groups) |

The PascalCase names are NetMaestro-internal DB model names; the file uses the
lowercase `snake_case` keys.

---

## 3. File shape

A complete configuration has these top-level blocks:

```yaml
schema_version: 1

simulation:        # run-level settings (mostly derived; a few user knobs)
components:        # the component configs (ComponentConfigs) referenced by the topology
topology:          # the Topology: an enumerated graph (nodes + links + groups) or a parametric fabric
jobs:              # optional: what workloads run and where — multi-job; see §6
surrogate:         # RESERVED: fast network surrogate; see §7
```

When NetMaestro exports a `Topology` for a run, it serializes the **components its
nodes reference** into `components:`, the run settings into `simulation:`, and the
graph into `topology:`. `jobs:` and `surrogate:` are **authored separately from the
topology graph** — they are run concerns, not something drawn in the topology editor —
and `jobs:` may be omitted in favour of a single inline component workload (§6.1).

The topology graph may be inline or in a separate file:

```yaml
topology:
  format: cytoscape
  file: my-network.json        # OR inline:
  # elements: { nodes: [...], edges: [...] }
```

Both Cytoscape forms are accepted: the object form `{ nodes: [...], edges: [...] }`
and the flat array form `[ { group: "nodes", ... }, ... ]`.

---

## 4. Custom components (`components:`)

A custom component pairs a `ComponentModel` with a configuration. Nodes reference it
by key.

```yaml
components:
  compute_host:                 # the component's key (referenced by nodes)
    model: nw-lp                # ComponentModel (required)
    type: host                  # ComponentType (optional; inferred from model)
    workload:                   # model-specific parameter blocks
      traffic: uniform
      num_messages: 30          # count — always a bare number
      arrival_time: 1000        # bare uses the default unit (ns); write "1us" to set it explicitly
      payload_size: 2048        # bare uses the default unit (bytes); write "2KiB" to set it explicitly
  edge_router:
    model: simplep2p
    type: router
    # routing: minimal          # user-facing knob with a model default — override here
    # chunk_size, vc_size, ...  # advanced knobs with defaults — override here
```

- **Required:** `model`.
- **Optional:** `type` (inferred from the model when omitted), plus any of the
  model's parameters. Omitted parameters take the model's default.
- A parameter is *user-facing* (documented, prominent) or *advanced* (defaulted,
  reachable) — both are settable here. Only the **derived** set (§9) can never be set.

---

## 5. Topology (`topology:`)

### 5.1 Nodes (`TopologyNode`)

| `data` field | Required | Meaning |
|---|:---:|---|
| `id` | ✅ | unique, stable node name; the compiler maps it to an LP id |
| `component` | ✅ | key of an entry in `components:` — every node references one |
| *(any component param)* | – | per-node **override** of the component's value (e.g. `num_messages: 100`) |
| `parent` | – | this node's `TopologyGroup` (a compound-node parent id) — **reserved**, see §10 |

A node **always** references a `component`.

Ignored by CODES: `position`, `classes`, `style`, `selected`, `grabbable`,
`locked`, `scratch`, and any other non-allowlisted key.

### 5.2 Links (`TopologyLink`)

| `data` field | Required | Meaning |
|---|:---:|---|
| `id` | conventional | link name |
| `source`, `target` | ✅ | node `id`s; both must exist |
| `bandwidth` | model-dependent | link capacity (unit-bearing, e.g. `"100Gbps"`). **Per-edge** for explicit-topology/WAN models (e.g. simplep2p); for HPC/regular models it's set **per link-class** as a model parameter (`local_bandwidth`/`global_bandwidth`/`cn_bandwidth`), not per edge |
| `latency` | model-dependent | propagation delay (unit-bearing, e.g. `"5ms"`). **Required** for long-haul/WAN models where propagation dominates (e.g. simplep2p); **derived**, not a per-link input, for HPC/datacenter models (latency falls out of bandwidth + per-hop delays) |
| `directed` | – | marks a one-way edge (used for per-direction asymmetric links); see §5.3 |
| *(advanced)* | – | **reserved**: per-link VC/buffer overrides, routing `weight` |

**Link meaning is determined by endpoint `ComponentType`** (the compiler's
interpretation, not stated in the file):

- `(router|switch) — (router|switch)` → a **network link**; its `bandwidth`/`latency`
  populate the model's link table (what the simplep2p matrix files used to do).
- `host — (router|switch)` → an **attachment**; binds the host to its injection
  point. `bandwidth`/`latency` are optional here.

By default a link is **symmetric** (same `bandwidth`/`latency` both ways if specified), and
`source`/`target` are just its two endpoints — not an origin and destination.

### 5.3 Link bandwidth & latency directionality

> **The format supports all three levels below**, with **symmetric the
> default**. The others stay available even though most links won't use them — the
> format shouldn't express *less* than the models it feeds (simplep2p's matrix has
> per-direction *and* per-side entries). Presented here for team feedback on
> **representation** (the per-direction form) and on **when** per-side is actually used.

A `TopologyLink`'s `bandwidth`/`latency` (if needed per link for the model) can be specified at three levels of detail.

#### Case A — Symmetric link  *(default; clearly needed)*

Same bandwidth and/or latency in both directions — LAN, datacenter fabric, backbone
fiber (full-duplex). One undirected edge, one value each:

```yaml
- data: { id: e0, source: router0, target: router1, bandwidth: "10Gbps", latency: "5ms" }
```

#### Case B — Per-direction asymmetry: source→target ≠ target→source  *(supported)*

Real situations:
- **Asymmetric access links** — ADSL/cable/satellite/cellular, e.g. 100 Mbps down /
  10 Mbps up (asymmetric *by design*).

Two candidate forms (`to_target` = the `source`→`target` direction):

**B1 — one link, directional values:**
```yaml
- data: { id: e0, source: home, target: isp,
          bandwidth: { to_target: "10Mbps", to_source: "100Mbps" },   # up / down
          latency:   "12ms" }                                         # scalar = same both ways
```

**B2 — two directed edges, one per direction:**
```yaml
- data: { id: e0, source: home, target: isp, directed: true, bandwidth: "10Mbps",  latency: "12ms" }
- data: { id: e1, source: isp,  target: home, directed: true, bandwidth: "100Mbps", latency: "12ms" }
```

Trade-off: **B1** keeps a single physical-link object (natural for an asymmetric
access link); **B2** makes each direction a first-class edge.

> **Open (representation):** which form is canonical — B1, B2, or both? Primarily a
> **NetMaestro** editor call (how an asymmetric link is drawn and edited). The choice
> **carries through to Case C** (per-side mirrors the same form).

#### Case C — Per-side egress/ingress split: sender-rate ≠ receiver-rate *within one direction*  *(supported; advanced, rarely used)*

The legacy simplep2p matrix stores, *for each direction*, a separate **egress**
(sender-side) and **ingress** (receiver-side) value — so a single transfer is charged
serialization at two different rates. Per-side refines *within* a direction, so it
**mirrors B1/B2 and must match whichever B form is chosen**:

**C1 — one link, directional + per-side values** (extends B1):
```yaml
# egress = charged at the sender, ingress = charged at the receiver; latency uses the same nested shape
- data: { id: e0, source: host0, target: router0,
          bandwidth: { to_target: { egress: "50Mbps", ingress: "25Mbps" },
                       to_source: { egress: "50Mbps", ingress: "45Mbps" } } }
```

**C2 — two directed edges, each with per-side values** (extends B2):
```yaml
- data: { id: e0, source: host0,   target: router0, directed: true, egress_bandwidth: "50Mbps", ingress_bandwidth: "25Mbps" }
- data: { id: e1, source: router0, target: host0,   directed: true, egress_bandwidth: "50Mbps", ingress_bandwidth: "45Mbps" }
```

Per-side is where the nested form bloats — C2 (directed edges) stays noticeably flatter
than C1's nested maps, which is worth weighing in the B1-vs-B2 call.

This is the only place CODES exposes it, and not sure how to think of it in a real world scenario.

> **Supported** as advanced fields (so the format can fully express simplep2p's
> matrix), but off the common path — omit them and the compiler uses one rate per
> direction (`egress == ingress`). **Domain feedback wanted:** when, if ever, do you
> use an independent sender-side vs receiver-side rate? It's hard to tie to a physical
> link, so we want to hear the real scenarios.

#### Summary

| Case | Situation | Config sketch | Status |
|---|---|---|---|
| **A** symmetric | LAN / DC / backbone (full-duplex) | one undirected edge, one value each | **supported — default** |
| **B** per-direction | asymmetric access links (ADSL/cable/sat/cellular) | B1 directional values · or B2 two directed edges | **supported** — form TBD (NetMaestro) |
| **C** per-side | sender vs receiver rate (simplep2p matrix) | C1 nested egress/ingress · or C2 directed edges w/ `egress_*`/`ingress_*` | **supported** — advanced, rare; **form follows B** |

### 5.4 Groups (`TopologyGroup`)

A group is an optional set of related nodes (e.g. a site). It is expressed natively
as a Cytoscape compound node: a node is a group when other nodes name it via
`data.parent`. Groups are **reserved** in Phase 3 (parsed, not yet acted on beyond
single-network) and become the seam for **multi-network / multi-site composition** —
the modern replacement for legacy `@annotation` scoping (see §7 of the roadmap).

### 5.5 Parametric topology source (HPC fabrics)

Regular HPC fabrics (dragonfly, fattree, torus, slimfly) are **not** drawn
node-by-node. Their connectivity is fully determined by a handful of **shape
parameters**, so they use a different topology *source*: `format: parametric`.
Instead of `elements:` (an explicit node/edge graph), the block carries a single
**fabric** description.

```yaml
topology:
  format: parametric
  fabric:
    model: <network model>     # the fabric's network model; the topology KIND is fixed by it
    shape: { ... }             # the model's shape parameters (a sufficient, non-redundant set)
    links: { ... }             # per-link-CLASS bandwidth / vc_size
    routing: { ... }           # fabric-global routing
    packet_size: ...
    chunk_size: ...
    connections: { intra: ..., inter: ... }   # ONLY for file-enumerated models — see below
  hosts:
    component: <component key>  # the workload component that runs on every compute-node slot
```

**Why a `fabric` block instead of per-node `components:`.** A fabric's parameters
are per **link-class** (`local`/`global`/`cn`, or `link`/`cn` for fattree) and
fabric-global (routing) — they do not attach to individual nodes. And the topology
*kind* is inseparable from the `model`: a fattree layout only exists with the fattree
model; you cannot place arbitrary components into it. So the fabric names its network
`model` directly (from the same catalog `components:` draw from) rather than wrapping
a single-use config in a named component. The **host workload is still a `components:`
entry** — it genuinely repeats across every terminal — referenced by `hosts.component`.

**Shape: a sufficient, non-redundant input set.** Each model defines a canonical
minimal input set; the user supplies a *sufficient* set and nothing redundant. For
dragonfly-dally the Dally construction derives every count from two numbers:

```yaml
topology:
  format: parametric
  fabric:
    model: dragonfly-dally
    shape:
      router_radix: 7
      conn_between_groups: 1
    links:
      local:  { bandwidth: "2GiBps",  vc_size: "16KiB" }
      global: { bandwidth: "2GiBps",  vc_size: "16KiB" }
      cn:     { bandwidth: "2GiBps",  vc_size: "32KiB" }
    routing: { algorithm: minimal, minimal_bias: 1 }
    packet_size: "4KiB"
    chunk_size:  "4KiB"
    connections:
      intra: "conf/dragonfly-dally/dfdally-72-intra"   # current binary files (see below)
      inter: "conf/dragonfly-dally/dfdally-72-inter"
  hosts:
    component: compute_host
```

`router_radix: 7, conn_between_groups: 1` is the *entire* shape of the 72-terminal
fabric: `num_routers_per_group = (radix+1)/2 = 4`, `num_cns_per_router = 2`,
`num_global_channels = 2`, `num_groups = 9` all fall out of it (the same derivation the
generator script does). Writing those derived counts **as well** is over-specification —
a second value can only agree (noise) or conflict (a silent bug). The compiler
**rejects/warns on redundant or conflicting shape values**. A user may pin a different
*single* knob where it's still sufficient (e.g. `num_groups` instead of `router_radix`),
but never two names for the same quantity.

Models whose natural inputs are already minimal (fattree, torus, slimfly) have only one
form. Note the link classes are **model-specific** — dragonfly has `local`/`global`/`cn`,
fattree has `link`/`cn` — another reason the fabric is tied to its model:

```yaml
topology:
  format: parametric
  fabric:
    model: fattree
    shape:
      num_levels: 3
      switch_count: [32, 32, 16]   # per level
      switch_radix: [8, 8, 8]
      # tapering: 1.0              # advanced; default 1.0
    links:
      link: { bandwidth: "12.5GiBps", vc_size: "64KiB" }
      cn:   { bandwidth: "12.5GiBps", vc_size: "64KiB" }
    routing: { algorithm: adaptive }
    packet_size: "512B"
    chunk_size:  "512B"
  hosts:
    component: compute_host
```

**Connectivity generation (current scope).** Of the four families, only the
**dragonfly-custom / -dally / -plus** variants read an explicit wiring from external
**binary connection files** (`intra` / `inter`, directed router→router edge lists);
the others generate connectivity internally from the shape parameters. Near-term CODES
keeps that split unchanged:

- **Internally-generated** (torus, fattree, slimfly, regular dragonfly): the compiler
  emits the shape parameters; the model generates as today. No external files, no
  `connections:` block.
- **File-enumerated** (dragonfly-custom/-dally/-plus): the user still produces the
  binary files with the existing generator scripts (`scripts/dragonfly-*/`) and
  references them via `fabric.connections.{intra,inter}`. The compiler passes the paths
  through to the model, which `fread`s them at init as it does today.

A future **shared generator utility** (shape → Cytoscape elements, runnable *outside*
CODES so NetMaestro can both visualize a fabric and feed the simulation from one source)
is **reserved, not precluded** (§10, §13). It is deliberately out of near-term scope
because HPC is not this SBIR's focus; the parametric format above is designed so that
utility can be added later without changing the user-facing schema.

**Reducing the file-drift footgun now (recommended, not build-now).** The one real
hazard today is that a file-enumerated model needs the shape counts in *both* the
config *and* the binary files, kept consistent by hand. Cheapest mitigation that
doesn't build the full utility: have the existing generator script **also emit the
`fabric.shape` block** (the counts it already computes) and the `connections:` paths it
just wrote — so a single command produces the binary files *and* a matching fabric
snippet, nothing hand-copied. A compiler-side validation pass (read the files, check the
implied router/group counts against the declared shape) is a cheaper-still backstop.

---

## 6. Jobs & workloads (`jobs:`)

What a host *runs* is separate from what a host *is*. A component answers "what kind of
endpoint" (`model: nw-lp`); a **job** answers "what workload runs, and where." Jobs are
a distinct top-level block because real runs are **multi-job** — different workloads on
different subsets of nodes (a trace replay beside synthetic background traffic, several
MPI apps co-scheduled) — which a single per-component workload cannot express.

A **job** = a **workload source** (what runs) + a **placement** (which nodes) + a rank
count.

```yaml
jobs:
  - id: production
    workload:
      type: dumpi                       # trace replay
      trace: "traces/app1.dumpi"
    ranks: 256
    placement: { policy: contiguous }   # 256 contiguous compute-node slots
  - id: background
    workload:
      type: synthetic
      traffic: uniform
      num_messages: 30
      payload_size: "2KiB"
      arrival_time: "1us"
    ranks: 128
    placement: { policy: random }
    qos: 1                              # optional priority; maps to num_qos_levels
```

### 6.1 The single-workload shortcut

For the common case — one workload on every endpoint — a component may carry an inline
`workload:` instead, and the compiler desugars it to a single job placed on all of that
component's nodes. This keeps simple configs simple (it is the form used in §11's worked
example):

```yaml
components:
  compute_host:
    model: nw-lp
    workload: { traffic: uniform, num_messages: 30, payload_size: "2KiB" }
    # ≡ jobs: [ { workload: <this>, ranks: <all compute_host slots>, placement: all } ]
```

A component carries an inline `workload:` **or** the config has a `jobs:` block — not
both.

### 6.2 Workload sources

One `type:` discriminator selects the source; the rest of the block is that source's
parameters (the same knobs the workload generators read today). The catalog:

| `type:` | Key parameters | Backed by |
|---|---|---|
| `synthetic` | `traffic` (uniform / nearest-neighbor / …), `num_messages`, `arrival_time`, `payload_size` | the synthetic traffic LPs |
| `dumpi` | `trace` (file / prefix) | offline MPI trace replay |
| `swm` | app name + `config` (JSON) | SWM online |
| `union` | `config` | UNION online |
| `darshan` | `log` | Darshan I/O trace |
| `checkpoint` | `checkpoint_sz`, `wr_bw`, `total_checkpoints`, `mtti` | checkpoint/restart synthetic |
| `iomock` / `iolang` | `num_requests` / `request_size` / `type`, or `kernel_meta` | I/O mocks |

These replace today's `workload_type` PARAMS key plus the scattered trace-file and
command-line plumbing.

### 6.3 Placement

`placement` says which compute-node slots a job's ranks occupy. Two forms:

- **Policy (common):** `{ policy: contiguous | scatter | random }` — the compiler/jobmap
  generates the allocation, the way real allocators do (the legacy
  `allocation-cont.conf` is the contiguous policy). Large jobs don't enumerate thousands
  of ids.
- **Explicit (escape hatch):** `{ nodes: [host3, host7, …] }` — an exact node list,
  mirroring the legacy `alloc_file`.

### 6.4 Phasing

The `jobs:` **schema is defined now** so the plan is visible and NetMaestro can build
around it, but it lands in stages:

- **Phase 3 (near-term / SBIR):** the inline-workload shortcut and the `synthetic`
  source; simple explicit multi-job. Enough for the WAN / simplep2p work.
- **Later stage:** rich placement policies and trace-driven multi-job at scale — the
  compiler generates allocations (the policy forms of §6.3) onto the existing jobmap,
  the way the legacy `allocation-cont.conf` / `alloc_file` flow does, rather than
  introducing a second allocator.

---

## 7. Surrogate (`surrogate:`) — reserved

CODES can swap the detailed network model for a fast **surrogate** (an average-latency
or learned predictor) over part of a run. This maps to a top-level `surrogate:` block,
the successor to the legacy `NETWORK_SURROGATE` section:

```yaml
surrogate:                              # RESERVED — schema tracks active development
  enable: true
  predictor: average                    # average | torch-jit
  director_mode: at-fixed-virtual-times
  switch_timestamps: [ "10ms", "89ms" ]
```

**Status: reserved placeholder.** The surrogate is on the SBIR path and needed soon, but
it is still being taken beyond a prototype, so the field set above is **indicative, not
pinned** — it will firm up as the surrogate productionizes. It is captured here so the
format reserves a home for it; until then a surrogate run may still be configured the
legacy way. (There is also an `APPLICATION_SURROGATE` counterpart, similarly reserved.)

---

## 8. Units

Every dimensioned parameter has a **documented default unit**. A bare number is
interpreted in that unit; a **unit-bearing string** (`"1us"`, `"2KiB"`) overrides it
and is converted. Explicit units are **recommended** — especially for bandwidth,
where there is no safe convention — but not required. Only dimensionless **counts**
(`num_messages`, `num_routers`, repetitions) are inherently unitless.

| Quantity | Bare number means | Explicit forms | Internal target |
|---|---|---|---|
| latency / time | ns | `"5ms"`, `"10us"`, `"1.5ns"` | ns |
| size | bytes | `"2KiB"`, `"1500B"`, `"4MiB"` | bytes |
| bandwidth | *(units recommended)* | `"100Gbps"`, `"10Gbps"`, `"2.5GBps"` | per-model (CODES mixes GiB/s and MiB/s today) |

---

## 9. Derived values — never written in this file

The compiler computes these from the models and topology; a hand value would be
wrong, so they are rejected (or ignored with a warning) if present:

| Value | Derived from |
|---|---|
| `message_size` (ROSS event-blob size) | the size of the models' message union |
| `modelnet_order` | the set of network models present |
| `pe_mem_factor` | defaulted from the run |
| repetition / group counts | the topology and component placement |

For a **parametric fabric** (§5.5), `modelnet_order` is derived from the fabric
`model`, and the repetition / group / router counts come from the fabric `shape`. (For
the file-enumerated dragonflies the shape counts are genuine *inputs* the model needs —
not derived — and must stay consistent with the connection files; that consistency is
the footgun §5.5's generator-emits-the-shape recommendation removes.)

> Note: Initially these values will be implemented in the yaml format while we transition away.

---

## 10. What the new format covers vs. reserves vs. defers

The format replaces the whole legacy `.conf` (LPGROUPS + PARAMS + `@annotation`) plus
the matrix/connection files — but in stages. This is the scope map for the *entire*
config surface, not just topology.

**Covered now** (Phase 3, hand-authored, verified by equivalence):
- *Enumerated WAN / single-network topology* (simplep2p / simplenet / synthetic):
  `id`, `component`, per-node overrides, `source`/`target`, `bandwidth`, `latency` —
  symmetric, per-direction, and per-side egress/ingress all supported (§5.3), mapped
  onto simplep2p's matrix; component `model`/`type`/params.
- *Parametric HPC fabrics* (§5.5): the `fabric` block — `model`, `shape`, per-class
  `links`, `routing`, `packet_size`/`chunk_size`, and (for the file-enumerated
  dragonflies) the `connections:` paths to the **existing** binary files. Compiled to
  the PARAMS the HPC models already read; generation stays with the current scripts.
- *Single-workload jobs* (§6): the inline-workload shortcut and the `synthetic` source.
- *`@annotation`*: not user-facing — subsumed by components (the compiler may emit
  annotations internally to drive the existing mapping).
- *Advanced network knobs* slot in as component/fabric params with no new concept:
  `modelnet_scheduler` (incl. `priority` + its sub-options), `num_qos_levels` /
  `qos_bandwidth`, multi-rail / multi-plane (`num_rails`, `rail_select`, `tapering`,
  `rail_routing`) — prominent-vs-advanced as elsewhere.

**Defined, lands in a later phase:**
- *Multi-job / trace-driven workloads* (§6.4) → rich placement policies on the
  **existing jobmap** (a model-level concern, independent of the LP-mapper /
  connectivity work).
- *`parent` / `groups:` (`TopologyGroup`)* → multi-network / multi-site composition — the
  modern replacement for cross-cluster `@annotation` scoping (the `forwarder`-bridged
  heterogeneous configs live here).
- *Storage / I/O models* (`lsm`, `resource`) → additional `ComponentModel`s
  (`model: lsm` + params); the abstraction already fits — unaddressed scope, driven by
  the I/O workload sources in §6.2.
- *Surrogate* (§7) → reserved `surrogate:` block; schema firms up as it productionizes.

**Reserved** (parsed/known, not yet acted on):
- the **shared generator → Cytoscape elements utility** (§5.5) — near-term HPC
  connectivity comes from the current generators + binary files; **not precluded**.
- the **very-large / Internet-scale** generated topology source. A generator's *output*
  is these same Cytoscape elements.
- advanced per-link parameters (VC/buffer overrides, routing weights).

**Explicitly deferred (a decision, not an oversight):**
- *Output / sampling / instrumentation* (`lp-io`, `cn_sample_file` / `rt_sample_file`,
  sampling intervals, ROSS instrumentation) — a thin pass-through for now; unifying
  CODES-direct stats vs. the ROSS instrumentation callbacks is its own future work
  (roadmap §7.4 / §10).

---

## 11. Worked example — simplep2p-style (2 routers, 4 hosts)

```yaml
schema_version: 1

simulation:
  end_time: "1ms"

components:
  compute_host:
    model: nw-lp
    type: host
    workload:
      traffic: uniform
      num_messages: 30
      arrival_time: "1us"
      payload_size: "2KiB"
  edge_router:
    model: simplep2p
    type: router

topology:
  format: cytoscape
  elements:
    nodes:
      - data: { id: host0,   component: compute_host }
      - data: { id: host1,   component: compute_host }
      - data: { id: host2,   component: compute_host }
      - data: { id: host3,   component: compute_host }
      - data: { id: router0, component: edge_router }
      - data: { id: router1, component: edge_router }
    edges:
      - data: { id: e0, source: host0,   target: router0, bandwidth: "100Gbps", latency: "1us" }
      - data: { id: e1, source: host1,   target: router0, bandwidth: "100Gbps", latency: "1us" }
      - data: { id: e2, source: host2,   target: router1, bandwidth: "100Gbps", latency: "1us" }
      - data: { id: e3, source: host3,   target: router1, bandwidth: "100Gbps", latency: "1us" }
      - data: { id: e4, source: router0, target: router1, bandwidth: "10Gbps",  latency: "5ms" }
```

This replaces the prototype's YAML + DOT + two matrix files, with no
`node_ids` lists, no `message_size`, and no separately-indexed latency/bandwidth
matrices.

---

## 12. Validation (compiler-side)

- node `id`s are unique;
- every link `source`/`target` resolves to a node;
- every node `component` resolves to a `components:` entry;
- every component `model` is a registered `ComponentModel`;
- required component parameters are present; units parse;
- (warning) a `router`/`switch` with no network link; a `host` with no attachment;
- (rejected/warned) any derived value (§9) written explicitly;
- a component carries an inline `workload:` **xor** the config has a `jobs:` block (§6.1);
- every job `workload.type` is a registered source; each job's `ranks` fit its placement;
  an explicit-`nodes` placement resolves to existing nodes of a workload-capable component.

---

## 13. Open items

- **Units strictness:** lenient (chosen) — every dimensioned parameter has a
  documented default unit; a bare number uses it, an explicit unit string overrides.
  Explicit units recommended (especially bandwidth).
- **Per-direction link form (§5.3):** all three directionality levels are supported;
  **open:** the canonical form for per-direction — B1 (one link, directional values)
  vs B2 (two directed edges), or both — a NetMaestro representation call.
- **Group semantics:** exact mapping of `TopologyGroup` → network/annotation scope
  (settled when multi-network lands).
- **HPC parametric source (§5.5):** drafted — `fabric` block, sufficient/non-redundant
  shape, per-class links, current generators + binary files kept. **Open:** the
  per-model canonical input sets (which single knobs are accepted); and the future
  **shared generator → elements utility** — *where it runs and in what language* (a
  standalone shared lib/service vs. one spec with native NetMaestro + CODES
  implementations). Deferred because HPC is not this SBIR's focus.
- **Jobs & workloads (§6):** schema sketched — `jobs:` block (workload source +
  placement + ranks), inline-workload shortcut, source taxonomy. **Open:** the placement
  policy set and its jobmap binding (on the existing jobmap, decoupled from the
  LP-mapper / connectivity work — §6.4); per-source parameter
  schemas beyond `synthetic`; whether per-job timing (start/pause/stop, the legacy
  timer/period files) is a job field or a separate block.
- **Surrogate (§7):** reserved `surrogate:` placeholder; field set indicative. Firms up
  as the surrogate moves beyond prototype — needed soon (SBIR), not yet pinned.
- **Model self-description / import:** how a third-party model (separate repo)
  advertises its `ComponentType`, parameters (name, type, default, unit, user-facing?)
  and connectivity so NetMaestro can ingest it into the catalog. Tracked with the
  Phase 4 model-framework work (see roadmap §10).
