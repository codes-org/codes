# CODES coding conventions

The conventions a CODES contributor needs in one place: how to name things, which
file extension to use, how headers are guarded, and how Logical Process (LP) state
must be constructed. Formatting (braces, spacing, include ordering, line length)
is **machine-checked** — see [Formatting](#formatting) — so this doc covers the
rules a formatter can't enforce.

## Naming

**`snake_case` throughout**.
This applies to functions, variables, struct/class names, and file names alike.
The public CODES C surface keeps its existing `codes_`-prefixed names.

## File extensions

| Extension | Use for |
| --- | --- |
| `.cxx` | C++ source files |
| `.c`   | C source files |
| `.h`   | Headers — with `extern "C"` guards where the header is C-includable |

The historical `.C` C++ extension has been retired (all `.C` files were renamed to
`.cxx`). `.cxx` also avoids the `.C`/`.c` collision on case-insensitive macOS
filesystems. Use `.cxx` for any new C++ translation unit; do not reintroduce `.C`.

A header that is included from C translation units must wrap its declarations in
`extern "C"` guards so it stays C-includable.

## Header guards

Use the existing CODES `#ifndef` include-guard pattern — **not** `#pragma once`:

```c
#ifndef CODES_<SUBSYSTEM>_<FILE>_H
#define CODES_<SUBSYSTEM>_<FILE>_H
/* ... */
#endif /* CODES_<SUBSYSTEM>_<FILE>_H */
```

Installed/public headers under `codes/` use the `CODES_..._H` form; internal
headers under `src/` use the `SRC_..._H` form mirroring their path. Match whichever
form the surrounding directory already uses.

## Formatting

Brace style, pointer placement, indentation, include ordering, and the soft
~100-column line length are owned by [`.clang-format`](../../.clang-format) at the
repo root — the **machine-checkable source of truth**. Don't restate or hand-apply
those rules; run the formatter (using v20):

```bash
clang-format -i path/to/file.cxx
```

CI runs `clang-format --dry-run --Werror` on every PR and rejects any drift (see
[`ci.md`](ci.md) and the README "Code formatting" section for editor setup), so
unformatted code doesn't merge.

## Documentation comments

Public/installed API — the headers under `codes/` — gets `/** … */` Doxygen
comments. These are added **incrementally, as code is touched**: there is
**no mass backfill**. The generated docs grow with the code rather than as a one-time
documentation sprint.

One Doxygen gotcha worth knowing because most of the public API is **C free
functions**: a documented function, variable, macro, or typedef only appears in
the output if its **enclosing header also carries a `/** @file */` block**. (C++
classes and namespaces show up without one.) So when you first document a header,
give it a one-line `@file` block at the top:

```c
/** @file
 *  Brief description of what this header provides.
 */
```

Then each `/** @brief … */` on a function in that header renders. Without the
`@file` block the symbol comment is silently dropped (a consequence of
`EXTRACT_ALL = NO`, not a bug).

Build the API docs locally (off by default):

```bash
cmake -S . -B build -DCODES_BUILD_DOXYGEN=ON
cmake --build build --target docs
```

Generated HTML lands in the **build tree** (`build/doc/html/`), which is already
git-ignored via `/build*` — it is never written into the source tree. The
Doxygen build uses `EXTRACT_ALL = NO`, so the output contains only entities that
actually carry a `/** */` comment: it starts near-empty by design and fills in as
comments are added. See [`doc/CMakeLists.txt`](../CMakeLists.txt) for the wiring.

## LP state: rich, but constructed

C++ members — including STL containers (`std::set`, `std::vector`, `std::map`, …)
— **are allowed** in LP state. But the state blob must be a *properly constructed*
C++ object:

- **Construct in `init`** (placement-new the state object into the ROSS-provided
  memory), and
- **Destruct in `final`** (run the destructor).

**Never assign a C++ object into zero-initialized ROSS memory.** ROSS hands a model
a zeroed state blob; assigning `s->ports = std::set<int>()` into never-constructed
memory is undefined behavior (the destructor of the moved-from temporary, and later
operations on the container, run against a never-constructed object). This has been
the root cause of some test segfaults and CI flakiness.

TODO: a `make_lptype<T>()` trampoline will make this **automatic** —
it placement-news the state object in `init` and runs its destructor in `final`, so
the state simply *is* a constructed C++ object. Until that lands, the tactical fixes
already applied pattern-match this rule by hand: placement-new
in `init`, explicit destructor call in `final`. Follow that pattern for any new
LP that carries C++ members.

**Messages stay POD.** ROSS `memcpy`s events, so event/message structs must remain
plain-old-data — no C++ members with non-trivial construction.
