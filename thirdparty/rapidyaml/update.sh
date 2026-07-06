#!/usr/bin/env bash

#=============================================================================
# Re-import the vendored RapidYAML amalgamation from upstream.
#
# Usage (run from anywhere in the checkout):
#
#     ./thirdparty/rapidyaml/update.sh
#
# This follows the Kitware third-party update convention: it sets the variables
# below and defines extract_source, then sources the shared update-common.sh,
# which clones upstream at $tag and merges the extracted tree onto the
# $subtree/ subtree as a single import commit.
#
# To move to a new version: change $tag below — a release tag OR any branch/commit
# SHA — and re-run. Nothing else needs editing: the amalgamation is regenerated in
# place under the stable name ryml_all.hpp, which codes_ryml.hpp includes.
#=============================================================================

set -e
shopt -s dotglob

readonly name="rapidyaml"
readonly ownership="RapidYAML Upstream <robot@codes>"
readonly subtree="thirdparty/rapidyaml/rapidyaml"
readonly repo="https://github.com/biojppm/rapidyaml.git"
readonly tag="v0.15.2"
readonly shortlog="false"
# The imported tree is the generated amalgamation, not a subset of upstream's own
# tree, so tree-object matching against upstream can't find the previous import;
# fall back to log-based matching on the import commit message.
readonly exact_tree_match="false"

extract_source () {
    # RapidYAML's single-header amalgamation is a generated artifact (upstream does
    # not commit it to their tree). Rather than downloading the pre-built header from
    # a GitHub *release* — which exists only for tagged versions — regenerate it from
    # the checked-out ref with upstream's own tool, so $tag can be a release tag OR an
    # arbitrary branch/commit (e.g. an unreleased upstream fix).
    #
    # tools/amalgamate.py is pure python3 (standard library only) and imports its
    # helpers from ext/c4core, so it needs nothing but python3 plus the recursive
    # checkout update-common.sh already produces. Its defaults (c4core + fastfloat +
    # stl, the tree event handler) are exactly what the released amalgamation ships,
    # so the output matches the release artifact. The LICENSEs live in the tagged
    # checkout (c4core is a submodule); extract_source runs with that checkout as its
    # working dir.
    mkdir -p "$extractdir/$name-reduced"
    cp -v LICENSE.txt            "$extractdir/$name-reduced/LICENSE.txt"
    cp -v ext/c4core/LICENSE.txt "$extractdir/$name-reduced/LICENSE.c4core.txt"
    python3 tools/amalgamate.py "$extractdir/$name-reduced/ryml_all.hpp"
}

. "${BASH_SOURCE%/*}/../update-common.sh"
