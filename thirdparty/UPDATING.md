# Updating Third Party Projects

CODES vendors a small number of third-party libraries under `thirdparty/`. Each
one is a reduced copy of an upstream project, imported with the Kitware
third-party update convention so that every change we carry is tracked as an
explicit, diffable commit rather than an ad-hoc edit.

This document explains how that machinery works: the
directory layout, how to re-import a project at a new version, and how to add a
new one.

## The two scripts

The update machinery is split in two:

- **`update-common.sh`** — the shared engine. It does all the real work: clone
  upstream, check out the requested version, run the package's extraction, and
  merge the result into our tree as a single import commit. This file is
  **Kitware's, vendored verbatim** (see the license header). Do **not** fork or
  hand-edit it — keeping it pristine is what lets us pull in upstream fixes to
  the convention itself with a clean diff.
- **`<pkg>/update.sh`** — a thin per-package driver. It sets a handful of
  metadata variables, defines an `extract_source` function, and then sources
  `update-common.sh`. This is the only script you normally edit, and usually the
  only line you touch is `tag`.

## Anatomy of a vendored package

Each package directory is **nested**, and the distinction is important:

```
thirdparty/
  googletest/            <- CODES-maintained "wrapper" directory
    CMakeLists.txt       <-   thin wrapper exposing a codes::thirdparty::<name> target
    README.md            <-   package-specific notes
    update.sh            <-   the per-package driver
    googletest/          <- the imported subtree (pristine upstream, do not edit)
      CMakeLists.txt
      include/  src/  ...
```

The **outer** directory (`thirdparty/<pkg>/`) holds files CODES maintains: the
wrapper `CMakeLists.txt`, the `README.md`, the `update.sh` driver, and any glue
code (for example rapidyaml's `codes_ryml.hpp` shim and `ryml.cpp` translation
unit). These are **preserved across re-imports**.

The **inner** directory (`thirdparty/<pkg>/<pkg>/`) is the imported subtree —
this is the `subtree` variable in `update.sh`. It is a pristine drop of upstream
and is **replaced wholesale** on every import.

**Never edit files in the inner subtree directly.** They will be overwritten on
the next re-import, and the change will be lost. Changes belong upstream (see
below); local glue belongs in the outer wrapper directory.

## Prerequisites

- **Git 2.5 or newer** — the engine uses `git worktree` to make the imported
  commits available to the main checkout.
- **`python3`** — for projects whose `extract_source` generates code. RapidYAML,
  for example, regenerates its single-header amalgamation with upstream's
  `tools/amalgamate.py` (standard library only; no extra packages).
- **`git-archive-all`** — only for projects that vendor a subset of an upstream
  tree that *contains submodules*, via the engine's `git_archive_all` helper.
  No current packages need it. Install with `pip install git-archive-all`
  and make sure the installed script (often `$HOME/.local/bin`) is on your
  `PATH`.

## Updating an existing project

### 1. Get the change upstream first

Any *code* change to a vendored library should first go to the upstream project,
using whatever workflow they require. Only once it is accepted do we bring it
into CODES. If an important fix stalls upstream, we can temporarily point `tag`
at a fork/branch carrying the patch, but the goal is always to track upstream.

### 2. Bump the version and re-import

The pinned version lives in exactly one place: the `tag` variable in the
package's `update.sh`. It is usually a release tag but may be `master`, a branch,
or any commit SHA.

```sh
# from the repo root
$ git checkout -b update_ryml_YYYY_MM_DD
# edit `tag` in thirdparty/rapidyaml/update.sh, then commit that edit
$ git commit -am 'thirdparty: bump rapidyaml to <version>'
# now run the driver
$ ./thirdparty/rapidyaml/update.sh
```

`update.sh` creates the import commit(s) for you. Dating the branch name is not
required; it just avoids collisions if you have an old update branch sitting around.

### 3. Validate, then open a PR

Re-configure and build to confirm the new drop compiles, and run the relevant
tests (for example, a `BUILD_TESTING` build exercises GoogleTest). Then review
the import commit and open a pull request from the branch as normal.

## Adding a new project

Create the package's outer directory and write its `update.sh`, filling in the
metadata the engine expects (all documented at the top of `update-common.sh`):

- `name` — the project name.
- `ownership` — a git author `Name <email>` for the import commits. Use the
  `"<Name> Upstream <robot@codes>"` convention. **Keep this stable across
  imports** (see "How re-import finds the previous drop" below).
- `subtree` — where the import lands, i.e. the nested
  `thirdparty/<name>/<name>` path.
- `repo` — the upstream git URL.
- `tag` — the version to import.
- `shortlog` — optional; `true` to include an upstream shortlog in the commit
  message.
- `exact_tree_match` — see below; reduced or generated imports must set this to
  `false`.

The key piece is the **`extract_source`** function, run inside a checkout of
upstream at `tag`. It must place the desired tree at `$extractdir/$name-reduced`.
If you only need to keep a subset of upstream's files verbatim, set the `paths`
variable and call the provided `git_archive` helper (or `git_archive_all` for
submoduled upstreams). If you need to generate or transform files, do that
instead (see rapidyaml's `update.sh` for a generated example).

Make `update.sh` executable and stage it before committing:

```sh
$ chmod u+x thirdparty/<name>/update.sh && git add thirdparty/<name>/update.sh
```

Finally, add the package to `thirdparty/CMakeLists.txt` and give it a wrapper
`CMakeLists.txt` and `README.md`.

## How it works under the hood

When you run a package's `update.sh`, the engine:

1. Clones upstream (recursively, for submodules) and checks out `tag`.
2. Runs your `extract_source`, producing `$name-reduced`.
3. Commits that tree as a **single squashed import commit** on a temporary
   `upstream-<name>` branch, authored by `ownership` and dated to match
   upstream.
4. Merges that branch onto the real tree under `subtree/` (a subtree merge for
   an existing package, or an initial subtree read for a brand-new one) and
   deletes the temporary branch.

### The import-commit contract

Import commits have a summary of the form `name YYYY-MM-DD (shorthash)`. This
is not cosmetic — re-import uses it to locate the previous drop (see below), so
don't reword these commits.

### How re-import finds the previous drop

To merge a new version onto the old one, the engine has to find the previous
import commit. It does this by searching history for commits authored by
`ownership` whose message matches the `name YYYY-MM-DD (hash)` pattern. Two
consequences:

- **`ownership` must stay identical between imports.** Change it and the engine
  can't find the prior drop; it will treat the re-import as a fresh initial
  import (or fail outright).
- **`exact_tree_match`** controls how strictly the match is made. The default
  (`true`) additionally requires the committed tree object to match what's
  currently in the checkout — correct only when you vendor upstream's tree
  *verbatim*. If `extract_source` vendors a **reduced subset** or **generates**
  files (as both current packages do), the trees won't match, so you must set
  `exact_tree_match=false` to fall back to log-based matching. The trap is
  delayed: the initial import succeeds regardless, and only the *first
  re-import* fails with "No previous import commit found" if this is left at the
  default.
