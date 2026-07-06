# RapidYAML

A reduced copy of [RapidYAML][ryml] (ryml) vendored into CODES to back the
YAML/JSON configuration front-end.

See [../UPDATING.md](../UPDATING.md) for the vendoring layout and how to
re-import. In short: do not edit the imported `rapidyaml/` subtree directly; the
pinned version is the `tag` in `update.sh`, and re-importing is `./update.sh`
after bumping it.

## How CODES builds it

A single-header amalgamation must have its implementation compiled in exactly one
translation unit. `ryml.cpp` is that TU (it defines `RYML_SINGLE_HDR_DEFINE_NOW`
and includes the header), and the wrapper `CMakeLists.txt` compiles it straight
into the `codes` library — `codes` is an installed/exported static library, so it
can't link a separate in-tree lib without dragging it into the export set.
Consumers just `#include <codes_ryml.hpp>`, the stable shim that forwards to
`ryml_all.hpp`.

[ryml]: https://github.com/biojppm/rapidyaml
