#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   ci/tidy.sh <build directory>
#
# Clang's static analyzer over every file of this project that the build in
# <build directory> compiles, on that build's own compile commands, and a
# failure for anything it reports.
#
# **It is what `/analyze` was.** While MSVC compiled this tree, its analyzer ran
# inside every compile and its findings were errors under /WX: a null
# dereference after an unchecked allocation, a buffer read past what was
# written. Clang has the same kind of analysis -- path-sensitive, one function
# and its callees at a time -- but not inside the compile, so it runs here, as a
# job of every push, and reads the same files with the same options. The checks
# are the analyzer's and nothing else: `clang-analyzer-*`, every one an error,
# less the analyzer's own opt-in set, `optin.*`. Those are opt-in upstream for
# the reason they would be here: `Padding` is about how a struct is laid out,
# not whether it is right, and `EnumCastOutOfRange` counts every value outside
# the enumerators, which for an enumeration with a fixed underlying type -- all
# of module.h's -- is a value it may hold, and one the ABI reads as unknown. The
# broader .clang-tidy at the root is for an editor, and says things about how
# code looks; this asks only whether it can go wrong.
#
# Only this project's own code is asked to be clean: src/, modules/, tests/ and
# shell/. external/ is other people's, and the analyzer reads its headers only
# because ours include them -- and FetchContent's Catch2 under the build
# directory the same. A fault of theirs is found by the fuzzers and the
# sanitizers, which read what the code does.
#
# clang-tidy reads each file with the options the build compiles it with, so the
# build has to have run first: several modules include headers that an external
# project generates. A file compiled for several configurations or targets has
# a command for each, and the Debug one is read -- it has the measuring code in
# it (MEDIAPERCH_DIAGNOSTICS), which Release leaves out.
set -euo pipefail

build=${1:-}
if [ -z "$build" ] || [ ! -f "$build/compile_commands.json" ]; then
    echo "usage: ci/tidy.sh <build directory with compile_commands.json>" >&2
    exit 2
fi

root=$(pwd -P)
# On Windows the paths are Windows paths with forward slashes, and Python is
# `python`: `python3` there may be the Store's placeholder that installs nothing.
case "${OSTYPE:-}" in
    msys*|cygwin*)
        root_pattern=$(cygpath --mixed "$root")
        python=$(command -v python)
        ;;
    *)
        root_pattern=$root
        python=$(command -v python3)
        ;;
esac

# The analyzer of the Clang this build was checked against: clang-tidy and
# run-clang-tidy are beside it in every LLVM release.
clang_tidy=$(command -v clang-tidy)
run_clang_tidy=$(dirname "$clang_tidy")/run-clang-tidy

# The commands of our files, one per file, and the pattern that names them.
# **Either separator, in the pattern**: on Windows run-clang-tidy normalises a
# path to backslashes and Clang reports a header as its include directory
# spelled it, forward slashes and a backslash after, so a pattern with `/` in it
# matched nothing -- the first run here read no file at all and passed.
commands=$(mktemp -d)
trap 'rm -rf "$commands"' EXIT
"$python" - "$build/compile_commands.json" "$commands" "$root_pattern" <<'REDUCE'
import json, os, re, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
root = sys.argv[3].replace("\\", "/").rstrip("/")
sep = r"[/\\]"
pattern = "^" + sep.join(re.escape(part) for part in root.split("/")) + \
    sep + "(src|modules|tests|shell)" + sep
ours = re.compile(pattern, re.IGNORECASE)
chosen = {}
for entry in entries:
    path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
    if not ours.match(path):
        continue
    output = entry.get("output", "").replace("\\", "/")
    debug = "/Debug/" in output
    if path not in chosen or (debug and not chosen[path][0]):
        chosen[path] = (debug, entry)
json.dump([e for _, e in chosen.values()],
          open(os.path.join(sys.argv[2], "compile_commands.json"), "w", encoding="utf-8"))
open(os.path.join(sys.argv[2], "pattern.txt"), "w", encoding="utf-8").write(pattern)
print("== %d compile commands, %d files of ours" % (len(entries), len(chosen)))
REDUCE
ours=$(cat "$commands/pattern.txt")

jobs=$(nproc 2>/dev/null || echo 4)
echo "== $("$clang_tidy" --version | sed -n 's/^.*LLVM version/LLVM/p' | head -n 1), $jobs at a time"

# Every file in the reduced database is ours, so none is named; the pattern
# is the header filter, anchored on the root and matched against the whole
# path, so the headers of external/ and of the build directory are read and
# not reported.
"$python" "$run_clang_tidy" -p "$commands" -j "$jobs" -quiet -use-color=0 \
    -clang-tidy-binary "$clang_tidy" \
    -config="{Checks: '-*,clang-analyzer-*,-clang-analyzer-optin.*', WarningsAsErrors: '*'}" \
    -header-filter="$ours"
