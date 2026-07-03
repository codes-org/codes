#!/usr/bin/env bash

#=============================================================================
# Re-import the vendored GoogleTest source from upstream.
#
# Usage (run from anywhere in the checkout):
#
#     ./thirdparty/googletest/update.sh
#
# Follows the Kitware third-party update convention: it sets the variables below
# and defines extract_source, then sources the shared update-common.sh, which
# clones upstream at $tag and merges the extracted tree onto the $subtree/ subtree
# as a single import commit.
#
# We vendor a *reduced* copy — the upstream repo root plus the googletest/ library
# subdirectory — and drop googlemock/, the library's own test/ and samples/, docs,
# CI, and bazel files. gmock is disabled at build time (BUILD_GMOCK=OFF in the
# wrapper CMakeLists), so the googletest/ subdirectory alone is sufficient.
#
# To move to a new release: bump $tag here, then re-run.
#=============================================================================

set -e
shopt -s dotglob

readonly name="googletest"
readonly ownership="Googletest Upstream <robot@codes>"
readonly subtree="thirdparty/googletest/googletest"
readonly repo="https://github.com/google/googletest.git"
readonly tag="v1.17.0"
readonly shortlog="false"
# The imported tree is a reduced subset of upstream's tree, so tree-object matching
# against a full-upstream import can't find the previous import; fall back to
# log-based matching on the import commit message.
readonly exact_tree_match="false"

extract_source () {
    # Keep the repo root (for its CMakeLists + LICENSE + README + CONTRIBUTORS) and
    # the googletest/ library subdirectory (its CMakeLists, cmake/, include/, src/).
    # Everything else — googlemock, the library test/ and samples, docs, CI, bazel —
    # is intentionally excluded. git archive honors export-ignore gitattributes.
    local paths="CMakeLists.txt LICENSE README.md CONTRIBUTORS \
        googletest/CMakeLists.txt googletest/README.md \
        googletest/cmake googletest/include googletest/src"
    git archive --worktree-attributes --prefix="$name-reduced/" HEAD -- $paths | \
        tar -C "$extractdir" -x
}

. "${BASH_SOURCE%/*}/../update-common.sh"
