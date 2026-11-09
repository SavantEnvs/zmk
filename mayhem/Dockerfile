# syntax=docker/dockerfile:1.7
#
# Commit-image Dockerfile (SMALL form, PREFERRED) — builds ON TOP of the org C/C++ base
# image (ghcr.io/savantenvs/base). The base already provides debian + clang/llvm +
# libFuzzer/afl++ + the identities + the build-contract ENV, so this only adds the
# per-commit build (built on top of the prebuilt base, rather than building the toolchain in-image).
#
# Build (from the repo root):
#   DOCKER_BUILDKIT=1 docker build -f mayhem/Dockerfile \
#       --build-arg COMMIT=$(git rev-parse HEAD) \
#       [--build-arg SOURCE_DATE_EPOCH=$(date +%s)] \
#       -t mytag .

# The org base, pinned to a fixed DATE-tagged base (YYYYMMDD; shared by all integrations
# minted against it). Use the current dated tag; pin by digest in production: ...base@sha256:<digest>.
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
# SOURCE_DATE_EPOCH stays an ARG (available to build.sh); do NOT bake it as an ENV — an
# empty value breaks clang ("must be a non-negative decimal integer") both here and for
# any in-image rebuild. build.sh unsets it when empty.
ARG SOURCE_DATE_EPOCH

# Sanitizer override: the base exports SANITIZER_FLAGS (ASan+UBSan, halting) as the default.
# To change it for a build (e.g. ASan-only, or add MSan): --build-arg SANITIZER_FLAGS="...".
# Declare it WITHOUT a default (no '='): an undefaulted ARG lets the base ENV flow through to
# build.sh when not passed, and an explicit --build-arg overrides it. (An empty default — `=` —
# would instead shadow the base ENV and silently wipe the sanitizers.)
ARG SANITIZER_FLAGS
# Coverage (optional, off by default): instrument the TEST build to measure how much of the project
# the suite exercises — --build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping".
# Empty by default (no effect on the normal oracle build); build.sh appends it to the test build only.
ARG COVERAGE_FLAGS=
# Build parallelism (optional). build.sh falls back to nproc when unset: --build-arg MAYHEM_JOBS=8
ARG MAYHEM_JOBS=

# ZMK is Zephyr firmware. The fuzz targets compile real ZMK sources on the host against real Zephyr
# headers, so the pinned Zephyr headers, nanopb and the studio protobuf messages are cached once here
# under /opt/toolchains/deps/<name>-<version>/ (SPEC 6.2 item 19); build.sh then runs fully offline.
# Every fetch is a shallow git fetch of one pinned commit, verified against the full sha. The nanopb C
# sources for the studio messages are generated here (same options as app/CMakeLists.txt), because the
# generator needs protoc, which is not needed again afterwards.
USER root
ARG ZEPHYR_REV=10ba6d0cb38bc3d258775d27982f707599320085
ARG NANOPB_REV=7307ce399b81ddcb3c3a5dc862c52d4754328d38
ARG STUDIO_MSGS_REV=6cb4c283e76209d59c45fbcb218800cd19e9339d
RUN [ -n "$SOURCE_DATE_EPOCH" ] && repro-sources-list.sh || true ; \
    apt-get update && apt-get install -yq --no-install-recommends protobuf-compiler python3-protobuf \
 && rm -rf /var/lib/apt/lists/*
RUN set -eu; \
    fetch() { d="$1"; url="$2"; rev="$3"; shift 3; mkdir -p "$d/src" && cd "$d/src" \
      && git init -q . && git remote add origin "$url" && git config core.sparseCheckout true \
      && if [ "$#" -gt 0 ]; then printf '%s\n' "$@" > .git/info/sparse-checkout; else echo '/*' > .git/info/sparse-checkout; fi \
      && git fetch -q --depth 1 --filter=blob:none origin "$rev" && git checkout -q FETCH_HEAD \
      && [ "$(git rev-parse HEAD)" = "$rev" ] && rm -rf .git; }; \
    z=/opt/toolchains/deps/zephyr-4.1.0-zmk-10ba6d0; \
    n=/opt/toolchains/deps/nanopb-1.0.0-dev-7307ce3; \
    m=/opt/toolchains/deps/zmk-studio-messages-6cb4c28; \
    fetch "$z" https://github.com/zmkfirmware/zephyr "$ZEPHYR_REV" /include/; \
    fetch "$n" https://github.com/nanopb/nanopb "$NANOPB_REV"; \
    fetch "$m" https://github.com/zmkfirmware/zmk-studio-messages "$STUDIO_MSGS_REV"; \
    protoc -I "$n/src/generator/proto" --python_out="$n/src/generator/proto" "$n/src/generator/proto/nanopb.proto"; \
    mkdir -p "$m/gen/proto/zmk" /tmp/zsm && cp "$m"/src/proto/zmk/* /tmp/zsm/; \
    sed 's/@CONFIG_ZMK_KEYMAP_LAYER_NAME_MAX_LEN@/20/' /tmp/zsm/keymap.options.in > /tmp/zsm/keymap.options; \
    cd /tmp/zsm && protoc --plugin=protoc-gen-nanopb="$n/src/generator/protoc-gen-nanopb" \
        -I /tmp/zsm -I "$n/src/generator/proto" --nanopb_out="-I/tmp/zsm:$m/gen/proto/zmk" \
        studio.proto meta.proto core.proto behaviors.proto keymap.proto; \
    rm -rf /tmp/zsm; chmod -R a+rX /opt/toolchains/deps; ls "$m/gen/proto/zmk"
USER mayhem

# Source enters from the build CONTEXT (the CI checkout): the FULL repo (incl. .git
# history) is copied to /mayhem as `mayhem`; build.sh builds in place.
USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

# Anything a Dockerfile step materializes into the tree (downloaded fixtures, generated headers,
# vendored deps) MUST be committed or (re)created by build.sh: rlenv's patch grader runs
# `git clean -ffdX` before every graded build, so a gitignored, untracked file is gone by then and
# every honest patch scores zero (SPEC §6.2 item 16; verify-repo.sh checks it).
#
# OPTIONAL compile-cache pre-fill (SPEC §6.2 item 18). Bases minted with /opt/mayhem/ccache/prefill
# run the build under rlenv's pinned ccache, so the patch grader's first (otherwise cold) rebuild is
# warm — this is what keeps large C/C++ targets inside the build time limit. Older bases fall back to
# the plain build; a missing or stale pre-fill only costs a cold cache. Never enable ccache in
# build.sh or via ENV/PATH, and keep the compiler resolving by name (CC=clang) or nothing is cached.
RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

# Run the project's functional tests at build time IF the repo ships them (mayhem/test.sh is
# optional — omit it only when the repo has no tests meaningful for RL). A failing suite fails the
# image build, so the commit image is guaranteed to build AND pass its own tests.
RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh — repo ships no functional tests"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
