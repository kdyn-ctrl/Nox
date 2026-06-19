# ── Stage 1: Builder ───────────────────────────────────────────────────────────────────────────────
# Compiles the execution engine binary inside a full build environment.
# The final image receives only the stripped binary — no compiler, no headers.
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    libssl-dev \
    ca-certificates \
    wget \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Download vendored single-header libraries — both pinned to explicit release tags.
# Issue #5 fix: httplib pulled from pinned v0.18.5 tag, NOT unpinned master.
# httplib: v0.18.5 | nlohmann/json: v3.11.3
RUN mkdir -p nlohmann \
    && wget -q -O httplib.h \
       https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.5/httplib.h \
    && wget -q -O nlohmann/json.hpp \
       https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp

COPY execution/main.cpp ./execution/
COPY shared/RegimeStateMachine.hpp ./shared/

# Issue #6 fix: compile with full C++17 + optimisation + required link flags.
# -std=c++17 is required for structured bindings and chrono literals used in httplib.
RUN cd execution && g++ -std=c++17 -I.. -O3 main.cpp -o nox_engine -lssl -lcrypto -lpthread

# ── Stage 2: Runtime ───────────────────────────────────────────────────────────────────────────────
# Minimal runtime image — only the binary and its shared library dependencies.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    ca-certificates \
    procps \
    && rm -rf /var/lib/apt/lists/*

# Run as non-root for container security — never execute trading logic as root.
RUN useradd --no-create-home --shell /bin/false appuser

WORKDIR /app
COPY --from=builder /app/nox_engine .
RUN chown appuser:appuser nox_engine

USER appuser

# Detect a hung/dead engine process.
HEALTHCHECK --interval=60s --timeout=5s --start-period=10s --retries=3 \
    CMD pgrep -x nox_engine > /dev/null || exit 1

EXPOSE 8080
CMD ["./nox_engine"]
