#!/usr/bin/env bash
#
# mayhem/build.sh — build this repo's fuzz harness(es). EDIT per repo.
#
# Runs inside the commit image (mayhem/Dockerfile) as `mayhem` in /mayhem. The base image
# (ghcr.io/savantenvs/base) already exports the build contract — use these, don't redefine:
#   CC, CXX             stock clang / clang++
#   LIB_FUZZING_ENGINE  -fsanitize=fuzzer   (link into each harness that has a LLVMFuzzer entry)
#   SANITIZER_FLAGS     -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
#                       (ASan + UBSan, both set to HALT — so Mayhem catches memory AND UB defects)
#   DEBUG_FLAGS         -g -gdwarf-3   (DWARF debug info — always on for fuzz/standalone builds,
#                       independent of the sanitizer off-switch; DWARF version must be < 4)
#   RUST_DEBUG_FLAGS    -C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3
#                       (thread through RUSTFLAGS on every cargo-fuzz build)
#   GO_DEBUG_FLAGS      -gcflags=all=-N -l
#                       (thread through go build / go-fuzz-build so the linked ELF keeps symbols)
#   SRC                 /mayhem (the repo source)
#
# Contract: build EVERYTHING here — one runnable binary per fuzz harness, AND the project's test
# suite (so mayhem/test.sh only has to RUN it, never compile). Keep it ADDITIVE (build upstream as
# upstream documents; don't edit upstream files). IMPORTANT: build the PROJECT ITSELF with
# $SANITIZER_FLAGS and $DEBUG_FLAGS (not just the harness) so the fuzzed code is instrumented
# AND carries DWARF < 4 symbols — otherwise ASan/UBSan only see the harness, not the library
# you're trying to find bugs in, and backtraces won't resolve project source lines. Build the TEST
# suite with the project's NORMAL flags (a clean, independent build) so test.sh stays an honest
# functional oracle and won't false-fail on benign UB. Leave the test binary/runner where test.sh
# expects it.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

# Build knobs come from the ENVIRONMENT (overridable), with sane defaults — no if-statements,
# just parameter-expansion fallbacks. Default sanitizers are the base's ASan+UBSan (halting); override
# per build via the Dockerfile's `--build-arg SANITIZER_FLAGS="..."`.
# NB: SANITIZER_FLAGS uses `=` (no colon) on purpose — `=` only fills when the var is UNSET, so an
# explicit EMPTY value (`--build-arg SANITIZER_FLAGS=`) is honored and builds with NO sanitizers
# (useful when you want the program's natural crash / full backtrace, not an ASan report). The other
# knobs use `:=` (default on empty too). MAYHEM_JOBS sets build parallelism (falls back to nproc).
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
# DEBUG_FLAGS carries DWARF debug info INDEPENDENTLY of the sanitizer off-switch (so an empty
# SANITIZER_FLAGS still yields DWARF symbols). DWARF MUST be < 4 (Mayhem triage can't read >=4); clang-19's
# plain `-g` emits DWARF-5, so `-gdwarf-3` is explicit. Apply $DEBUG_FLAGS to the fuzz/harness/standalone
# builds (NOT the test/oracle build). Rust/Go carry the same intent via their language flags below.
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${RUST_DEBUG_FLAGS:=-C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3}"
: "${GO_DEBUG_FLAGS:=-gcflags=all=-N -l}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
# COVERAGE_FLAGS: empty by default → no effect on the normal oracle build. Set it via the Dockerfile's
# `--build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping"` to instrument the TEST
# build for source-coverage measurement (how much of the project the test suite actually exercises —
# a quality signal for the oracle; complements the anti-reward-hack sabotage check). APPEND it to the
# test build's compile+link flags in step 3 (NOT the fuzz build); after `test.sh` runs, merge with
# `llvm-profdata` and report with `llvm-cov`. Empty value is honored (`=`, not `:=`).
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS RUST_DEBUG_FLAGS GO_DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# ZMK is Zephyr firmware, which needs a Zephyr/west toolchain to build as a whole. The fuzz targets
# instead compile real ZMK sources on the host against the real pinned Zephyr headers (cached in the
# image under /opt/toolchains/deps), with tiny shims in mayhem/shim for the kernel services those
# sources touch (logging, errno/stdint includes, version, Kconfig values). Everything here is offline.
DEPS=/opt/toolchains/deps
ZEPHYR="$DEPS/zephyr-4.1.0-zmk-10ba6d0/src"
NANOPB="$DEPS/nanopb-1.0.0-dev-7307ce3/src"
GEN="$DEPS/zmk-studio-messages-6cb4c28/gen"
B="$SRC/mayhem/build"
APP="$SRC/app"
SHIM="$SRC/mayhem/shim"
H="$SRC/mayhem/harness"

INC="-I$SHIM -I$ZEPHYR/include -I$APP/include -I$APP/src -I$NANOPB -I$GEN"
PBDEF="-DPB_NO_ERRMSG=1 -DPB_WITHOUT_64BIT=1"
NANOPB_SRCS="$NANOPB/pb_common.c $NANOPB/pb_decode.c $NANOPB/pb_encode.c"
GEN_SRCS="$(ls "$GEN"/proto/zmk/*.pb.c)"

rm -rf "$B"
mkdir -p "$B"

# compile <outdir> <source> <object name> <flags...>
compile() {
  local out="$1" src="$2" name="$3"; shift 3
  "$CC" "$@" $INC $PBDEF -c "$src" -o "$out/$name.o"
}

# build_set <outdir> <flags...>: every object the three fuzz binaries link, with the given flags.
build_set() {
  local out="$1"; shift
  mkdir -p "$out"
  for k in nkro hkro; do
    compile "$out" "$APP/src/hid.c" "hid_$k" "$@" -include "$SHIM/config_$k.h"
    compile "$out" "$H/hid_ops.c" "hid_ops_$k" "$@" -include "$SHIM/config_$k.h"
  done
  compile "$out" "$APP/src/studio/msg_framing.c" msg_framing "$@" -include "$SHIM/config_nkro.h"
  compile "$out" "$H/studio_rpc.c" studio_rpc "$@" -include "$SHIM/config_nkro.h"
  for f in $NANOPB_SRCS $GEN_SRCS; do
    compile "$out" "$f" "$(basename "$f" .c)" "$@"
  done
}

# 1+2) Fuzz binaries: the project sources (hid.c, msg_framing.c) AND nanopb with the generated studio
#      messages, which are the fuzz target for studio_rpc, are compiled with the fuzz flags. No sanitizer
#      check is relaxed.
"$CXX" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$B/lsan_off.o"

build_set "$B/fuzz" $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link
build_set "$B/standalone" $SANITIZER_FLAGS $DEBUG_FLAGS

PB_OBJS="pb_common.o pb_decode.o pb_encode.o $(for f in $GEN_SRCS; do printf '%s.o ' "$(basename "$f" .c)"; done)"
link() { # <flavor dir> <binary> <objects...>
  local dir="$1" bin="$2"; shift 2
  local objs=""
  for o in "$@"; do objs="$objs $B/$dir/$o"; done
  if [ "$dir" = fuzz ]; then
    "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE $objs "$B/lsan_off.o" -o "/mayhem/$bin"
  else
    "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS "$STANDALONE_FUZZ_MAIN" $objs "$B/lsan_off.o" -o "/mayhem/$bin-standalone"
  fi
}
for flavor in fuzz standalone; do
  link $flavor studio_rpc studio_rpc.o msg_framing.o $PB_OBJS
  link $flavor hid_nkro hid_ops_nkro.o hid_nkro.o
  link $flavor hid_hkro hid_ops_hkro.o hid_hkro.o
done

# 3) Known-answer test programs, built with the normal flags (no sanitizers, no -gdwarf-3, so the suite
#    is an independent functional oracle). mayhem/test.sh only runs them.
T="$B/kat"
mkdir -p "$T"
for k in nkro hkro; do
  "$CC" -O2 $COVERAGE_FLAGS $INC "-include" "$SHIM/config_$k.h" \
      "$H/kat_hid.c" "$APP/src/hid.c" -o "$T/kat_hid_$k"
done
"$CC" -O2 $COVERAGE_FLAGS $INC $PBDEF -include "$SHIM/config_nkro.h" \
    "$H/kat_studio.c" "$APP/src/studio/msg_framing.c" $NANOPB_SRCS $GEN_SRCS -o "$T/kat_studio"
