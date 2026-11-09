#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# Runs the known-answer programs mayhem/build.sh built from the real ZMK sources: HID report building
# (NKRO and HKRO configurations) and the Studio frame layer + Request messages. Each prints one
# `ok - <name>` / `not ok - <name>` line per assertion and a `KAT <name> passed=N failed=M` summary.
# Counts come from those lines; a program that is missing, crashes or prints nothing (for example a
# neutered binary) fails because its summary line is absent.
KAT="$SRC/mayhem/build/kat"
total_pass=0
total_fail=0
for prog in kat_hid_nkro kat_hid_hkro kat_studio; do
  if [ ! -x "$KAT/$prog" ]; then
    echo "missing $KAT/$prog: mayhem/build.sh did not build it" >&2
    total_fail=$((total_fail + 1))
    continue
  fi
  out=$("$KAT/$prog" 2>&1) ; rc=$?
  printf '%s\n' "$out"
  p=$(printf '%s\n' "$out" | grep -c '^ok - ')
  f=$(printf '%s\n' "$out" | grep -c '^not ok - ')
  summary=$(printf '%s\n' "$out" | grep -E '^KAT [a-z_-]+ passed=[0-9]+ failed=[0-9]+$' | tail -n1)
  want="KAT ${prog#kat_}"; want="${want//_/-} passed=$p failed=$f"
  if [ "$rc" -ne 0 ] || [ "$p" -eq 0 ] || [ "$summary" != "$want" ]; then
    echo "$prog: bad result (exit $rc, summary '$summary', expected '$want')" >&2
    f=$((f + 1))
  fi
  total_pass=$((total_pass + p))
  total_fail=$((total_fail + f))
done

emit_ctrf "zmk-known-answer" "$total_pass" "$total_fail" 0
