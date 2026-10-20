#!/bin/bash
# Host-only tests: no Mac, no card, no XRT install. They run on ace in seconds
# and under sanitizers, which is the point - the code they cover parses
# untrusted binary input and could previously only be exercised on an FPGA.
#
# Exit status is the verdict. Every test binary exits non-zero if anything
# failed, and prints one "TEST <name>: N passed, M failed" line.
set -u
cd "$(dirname "$0")"
CXX=${CXX:-g++}
SAN=${SAN:-address,undefined}
INC="-I. -Icompat -I.."
# -fno-sanitize-recover makes a UBSAN report abort instead of printing and
# carrying on. A misaligned load was reported and the run still said it passed,
# which defeats the point of having the check.
FLAGS="-std=c++17 -g -O1 -Wall -Wextra -fno-omit-frame-pointer -fsanitize=$SAN -fno-sanitize-recover=all"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

rc=0
for src in *_test.cpp; do
  name=${src%_test.cpp}
  # Each test links the shim source it covers; these are the files with no
  # IOKit or card dependency.
  case $name in
    fdt) deps="../fdt_macos.cpp" ;;
    *)   deps="" ;;
  esac
  if ! $CXX $FLAGS $INC -o "$out/$name" "$src" $deps > "$out/$name.build" 2>&1; then
    echo "BUILD FAILED: $name"
    sed -n '1,12p' "$out/$name.build"
    rc=1
    continue
  fi
  # ASAN's leak checker is on by default on Linux; keep it, it is free coverage.
  #
  # setarch -R disables address-space randomisation for the child. Without it
  # ASAN intermittently fails to place its shadow mapping on this kernel
  # ("Shadow memory range interleaves with an existing memory mapping"), which
  # aborts before any test runs and looks exactly like a failure.
  runner=""
  command -v setarch >/dev/null 2>&1 && setarch -R true >/dev/null 2>&1 && runner="setarch -R"
  if ! ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 $runner "$out/$name"; then
    rc=1
  fi
done

[ $rc -eq 0 ] && echo "ALL HOST TESTS PASSED" || echo "HOST TESTS FAILED"
exit $rc
